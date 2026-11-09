# Plugin s3rdma_ddn: S3-over-RDMA via NVIDIA cuObject

Contributed by DDN in [breuner/elbencho#123](https://github.com/breuner/elbencho/pull/123).

This plugin performs S3 PUT, GET and multipart part uploads using NVIDIA's cuObject
(`cuObjClient`, protocol `CUOBJ_PROTO_RDMA_DC_V1`). The object payload moves out-of-band over
RDMA (directly to/from GPU memory when `--gpuids` is given, otherwise host memory), while a small
body-less HTTP control request carries the RDMA token in an `x-amz-rdma-token` header. The control
requests are sent via libcurl with AWS SigV4 signing (`UNSIGNED-PAYLOAD`). All other S3 requests
(e.g. bucket creation, multipart upload create/complete, deletes) go through the AWS SDK as usual.

It requires an S3 endpoint that supports this protocol, such as DDN Infinia.

## Requirements

* NVIDIA cuObject client library (`libcuobjclient`) and cuFile (`libcufile`) from CUDA 13.1+
* libcurl with development headers (`libcurl-devel` / `libcurl4-openssl-dev`), version 7.75+ for
  `CURLOPT_AWS_SIGV4`
* RDMA-capable NICs (RoCEv2 or InfiniBand) on the client

No cuObject binaries are shipped with elbencho: they are NVIDIA proprietary and carry their own
EULA. The `libcuobjclient` version has to be compatible with the cuObject version of the S3
server.

## Building

```bash
make clean-all # required when changing any optional build feature or plugin
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_DDN=1
```

The cuObject headers and libraries are looked up under `/usr/local/cuda/targets/<arch>-linux`
(e.g. `x86_64-linux` or `sbsa-linux`). Set `CUOBJECT_ROOT_DIR` to use a different location:

```bash
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_DDN=1 \
  CUOBJECT_ROOT_DIR=/usr/local/cuda-13.1/targets/x86_64-linux
```

`bin/elbencho --version` lists `s3rdma_ddn` among the included plugins, and
`bin/elbencho --help-all` shows the plugin options.

## Running

The plugin is activated with `--plugins s3rdma_ddn`. Example (100 threads writing 100,000 8 MiB
objects each with a time limit of 30 minutes and per-second live stats in a CSV file):

```bash
CUFILE_ENV_PATH_JSON=/path/to/cuobj.json \
  elbencho -w --plugins s3rdma_ddn --s3endpoints https://S3SERVER:8111 \
    --s3region "$AWS_REGION" --s3key "$AWS_ACCESS_KEY_ID" --s3secret "$AWS_SECRET_ACCESS_KEY" \
    --s3nocompress --lat --cpu -t 100 -n 1 -N 100000 -s 8m -b 8m --timelimit 1800 \
    --liveint 1000 --livecsv put_live.csv --s3objprefix l50/ mybucket
```

Use `-r` instead of `-w` to read the objects back and add `--gpuids <id>` for transfers directly
to/from GPU memory. In distributed mode, the service instances need to be built with the plugin
as well.

**Runtime configuration:** cuObject reads a JSON config file (point `CUFILE_ENV_PATH_JSON` at it)
with the client NIC(s) and the RDMA properties, e.g.:

```json
{
  "properties": {
    "rdma_peer_type": "dmabuf",
    "rdma_dev_addr_list": ["<client-nic-ipv4>"],
    "rdma_multipath_enabled": true
  }
}
```

The further runtime notes in the [s3rdma_minio README](../s3rdma_minio/README.md) (multi-NIC,
`allow_compat_mode` without GPU, `libcufile_rdma.so` on `LD_LIBRARY_PATH`, matching RoCE MTUs)
apply here as well.

## Options

| Option | Default | Description |
|---|---|---|
| `--s3rdmatimeout <ms>` | 30000 | Timeout for a single RDMA request attempt, including connection setup and waiting for the response. |
| `--s3rdmaconntimeout <ms>` | 5000 | Timeout for establishing the connection of an RDMA request. |
| `--s3rdmaretries <n>` | 3 | Retries of a request that failed with a network error, timeout or HTTP 429/500/502/503/504. 0 disables retries. |
| `--s3rdmaretrydelay <ms>` | 100 | Delay before the first retry; doubles with every further retry, up to 2000ms. |

Every retry uses a fresh RDMA token. The connection of a failed attempt is discarded, so a retry
also uses a new connection and a fresh DNS lookup. This keeps a benchmark running when a single
server link goes down and in-flight requests on it never get a response.

At the end of a run, a summary line reports the number of request retries, requests that
succeeded after a retry and requests that failed. Combine the retries with `--s3ignoreerrors` to
keep running for fault injection tests even when a request still fails after all retries; such
requests are then only visible in this summary (and logged with `--log 1`).

## Constraints

* The RDMA control requests carry none of the per-request S3 options, so inline ACLs
  (`--s3aclputinl`), server-side encryption (`--s3sse*`) and checksum algorithms
  (`--s3chksumalgo`) are rejected while the plugin is active.
* The block size (`-b`) determines the size of the RDMA-registered buffers, so objects larger
  than the block size are uploaded as multipart uploads with each part sent over RDMA.
* An RDMA failure that persists after all retries is a hard error (unless `--s3ignoreerrors`);
  there is no automatic HTTP fallback.
* TLS certificates of the S3 endpoint are not verified (to allow self-signed certificates), same
  as for elbencho's regular S3 client.
* Linux only.

## Files

* `source/S3RdmaDdnPlugin.*`: the plugin registration, options and argument checks.
* `source/S3RdmaDdnTransport.*`: the `S3Transport` that routes PUT/GET and multipart part
  uploads over RDMA while the plugin is active, incl. the retry logic and the summary.
* `source/S3TkCurl.*`: the libcurl-based control plane (SigV4-signed requests with the RDMA
  token, connection cache) and the process-wide `cuObjClient`.
* `plugin.mk`: build rules and cuObject library detection.
