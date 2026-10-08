# Plugin s3rdma_minio: GPU-Direct S3-over-RDMA via NVIDIA cuObject

Contributed by Harshavardhana (MinIO) in
[breuner/elbencho#112](https://github.com/breuner/elbencho/pull/112).

This plugin performs S3 GET/PUT using NVIDIA's cuObject (`cuObjClient`) API, the object-storage
counterpart of `--cufile` (GDS). The object payload moves out-of-band over RDMA
(directly to/from GPU memory when `--gpuids` is given, otherwise host memory), while a small
body-less HTTP control request carries the `x-amz-rdma-*` protocol headers. It requires an
RDMA-capable S3 endpoint that implements that protocol.

## Building

```bash
make clean-all
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_MINIO=1
```

This requires the cuObject client library (CUDA 13.1+) and the RDMA verbs development libraries
(`libibverbs`, `librdmacm`). The library is located through its pkg-config module
(`cuobjclient-<major>.<minor>`), falling back to a search under `/usr/local/cuda*`;
`CUOBJ_INCLUDE_PATH` and `CUOBJ_LIB_PATH` override both. `S3_AWSCRT=1` is not supported.

No cuObject binaries are shipped with elbencho: they are NVIDIA proprietary and carry their own
EULA. Install the `libcuobjclient` development package that matches your S3 server's cuObject
version.

**Version compatibility:** the `libcuobjclient` that elbencho links must be compatible with the
cuObject version of your S3 server (e.g. 1.2.0 pairs with `libcuobjserver` 1.2.0). Do not mix
major/minor versions. A client/server mismatch typically surfaces as RDMA buffer-registration
failures or `retry exceeded` transfer errors even though the control-plane HTTP request succeeds.

## Running

The plugin is activated with `--plugins s3rdma_minio`. Example (16 threads, 8 MiB objects,
host/CPU buffers):

```bash
CUFILE_ENV_PATH_JSON=/path/to/cuobj.json \
LD_LIBRARY_PATH=/path/to/cuobject/lib \
  elbencho --s3endpoints https://S3SERVER:9000 --s3key KEY --s3secret SECRET \
    --plugins s3rdma_minio --iodepth 1 -w -r -t 16 -s 8m -b 8m s3://mybucket
```

Add `--gpuids <id>` for VRAM-direct transfers. The RDMA control request is pinned to HTTP/1.1, so
the `x-amz-rdma-*` header exchange doesn't depend on HTTP/2 negotiation. In distributed mode, the
service instances need to be built with the plugin as well.

**Runtime configuration:** cuObject reads a JSON config file (point `CUFILE_ENV_PATH_JSON` at it).
The client NIC(s) and RDMA properties must be set, for example:

```json
{
  "properties": {
    "allow_compat_mode": true,
    "use_pci_p2pdma": true,
    "rdma_peer_type": "dmabuf",
    "rdma_dev_addr_list": ["<client-nic-ipv4>"],
    "rdma_multipath_enabled": true
  }
}
```

**Multi-NIC clients:** NIC selection across multiple RDMA NICs is handled entirely by cuObject,
not by elbencho. List each client RDMA NIC IPv4 in `rdma_dev_addr_list` and set
`rdma_multipath_enabled: true`. cuObject then chooses the NIC, embeds its GID in the RDMA token (so
the server transfers to the correct interface regardless of which NIC the HTTP control request
used), and handles failover/failback across the listed NICs (see the `rdma_max_backup_devices`,
`rdma_io_retry_count`, `rdma_failback_enabled` and `rdma_health_check_interval_ms` properties).
With a single entry it transparently runs single-path.

## Constraints

* `--iodepth 1` is required, and `--s3fastget` cannot be used.
* The block size (`-b`) is limited to 4095 MiB, because cuObject cannot register a 4 GiB buffer.
  Objects larger than the block size are uploaded as multipart uploads with each block-sized
  part sent over RDMA (also with `--sharesize`), while the create, complete and abort requests go
  over regular HTTP. Downloads read each block as a ranged RDMA GET.
* The RDMA control requests carry none of the per-request S3 options, so inline ACLs
  (`--s3aclputinl`), server-side encryption (`--s3sse*`) and checksum algorithms
  (`--s3chksumalgo`) are rejected while the plugin is active.
* An RDMA decline or failure is a hard error; there is no automatic HTTP fallback.
* The host needs RDMA-capable NICs (RoCEv2 or InfiniBand) and a sufficiently high locked-memory
  limit (`memlock`). For VRAM-direct transfers (`--gpuids`), GPUDirect RDMA must be working between
  the GPU and NIC (e.g. PCIe ACS redirect disabled on the data-path bridges).
* `libcufile` loads `libcufile_rdma.so` with `dlopen`, which does not consult the executable's
  rpath. Put the directory holding it on `LD_LIBRARY_PATH`, or RDMA registration fails with
  `no devices found in configuration` even though `rdma_dev_addr_list` is set. The cuFile log
  reports this as `--rdma library : Not Loaded (libcufile_rdma.so)`.
* The RoCE MTU of the client NIC has to match the server's (e.g. both 9000, i.e. an RDMA MTU of
  4096): cuObject's DC transport does not negotiate it. With a mismatch, plain HTTP works while
  RDMA uploads fail with an HTTP 500 after the server's RDMA reads and RDMA downloads report
  success without delivering data. `ibv_devinfo` shows the `active_mtu`.
* On a client without the `nvidia_peermem` module (e.g. with the open kernel module, which uses
  dma-buf), set `rdma_peer_type: "dmabuf"`. cuFile otherwise disables userspace RDMA
  (`nvidia_peermem.ko is not loaded` in the cuFile log) and buffer registration fails with
  `no devices found in configuration`.
* On a client without a GPU or without `nvidia-fs.ko` loaded, set `allow_compat_mode: true`.
  cuFile otherwise refuses to initialize (`nvidia-fs.ko driver not loaded`) and the plugin reports
  the fabric as not connected. Host-memory RDMA transfers work normally in this mode.

## Testing without cuObject hardware

[tools/s3rdma](../../../tools/s3rdma/README.md) has a small S3-over-RDMA test server and a
drop-in replacement for NVIDIA's client library (`cuobjclient-shim`) that implements the cuObject
client API over Reliable Connections on plain `libibverbs`. With them, the plugin's RDMA path runs
on any RoCE NIC without a GPU and without DC transport support, e.g. for the test suite
(`tests/run-tests.sh -r`, which picks the shim automatically where NVIDIA's library cannot work).

`CUOBJ_SHIM=1` builds the plugin against the shim instead of NVIDIA's library (no CUDA toolkit
needed; `make clean` first when switching):

```bash
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_MINIO=1 CUOBJ_SHIM=1
```

An executable linked against NVIDIA's library picks up the shim at run time via
`LD_LIBRARY_PATH=tools/s3rdma/cuobjclient-shim/lib`. Either way, the shim is configured by
environment variables instead of a cuObject JSON config (`S3RDMA_RC_ENDPOINT`, `S3RDMA_RC_ADDR`,
see its README) and works only with the test server.

## Files

* `source/S3RdmaMinioPlugin.*`: the plugin registration and argument checks.
* `source/S3RdmaMinioTransport.*`: the `S3Transport` that routes PUT/GET and multipart part
  uploads over RDMA while the plugin is active.
* `source/CuObjClientTk.*`: process-wide `cuObjClient` singleton (buffer registration, tokens).
* `source/S3RdmaTk.*`: the signed HTTP control plane (`x-amz-rdma-*` headers, UNSIGNED-PAYLOAD
  SigV4) and the retry wrappers.
* `source/S3RdmaProtocol.h`: the protocol constants and reply parsing.
