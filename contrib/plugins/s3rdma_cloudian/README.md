# Plugin s3rdma_cloudian: S3-over-RDMA via Cloudian's aws-sdk-cpp fork

Contributed by Jonathan Teh (Cloudian) in
[breuner/elbencho#121](https://github.com/breuner/elbencho/pull/121).

This plugin builds elbencho against Cloudian's RDMA-enabled fork of the AWS SDK for C++. The fork
adds RDMA variants of the S3 object requests (`PutObjectRDMA`, `UploadPartRDMA`, `GetObjectRDMA`)
that transfer the object data via NVIDIA cuObject instead of the HTTP body. Unlike a plugin that
only adds code, this one **replaces the AWS SDK for the whole executable**: the RDMA request
types are always in use in such a build. While the plugin is not activated at runtime, the fork
falls back to TCP for them. `--s3fastget` is not available in such a build, because these
requests always need a target buffer.

## Requirements

1. Cloudian HyperStore 8.2.6.4
2. Cloudian [aws-sdk-cpp](https://github.com/cloudian/aws-sdk-cpp) 1.11.893+rdma
3. NVIDIA cuObject client 1.0.0 (CUDA Toolkit 13.1.1)

## Building

Either let elbencho's externals step clone and build the fork (instead of the stock AWS SDK):

```bash
make clean-all # the externals step keeps an existing AWS SDK build, so clean when switching
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_CLOUDIAN=1
```

Or build and install the fork as static libraries yourself and point elbencho at it:

```bash
git clone --depth 1 --recurse-submodules --shallow-submodules --branch 1.11.893+rdma https://github.com/cloudian/aws-sdk-cpp.git
cd aws-sdk-cpp
mkdir build
cd build
cmake .. -DBUILD_ONLY=s3 -DENABLE_TESTING=OFF -DBUILD_SHARED_LIBS=OFF
make -j$(nproc)
sudo make install
```

```bash
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_S3RDMA_CLOUDIAN=1 AWS_INCLUDE_DIR=/usr/local/include/ AWS_LIB_DIR=/usr/local/lib/
```

`docker/Dockerfile.ubuntu-cuda-s3rdma-multiarch.local` builds a container image this way.

## Running

RDMA is enabled with `--plugins s3rdma_cloudian`. This requires that the NVIDIA cuobjclient
library can be loaded and initialized (otherwise the run fails with an error) and that the S3
endpoint supports RDMA operations (otherwise the fork falls back to TCP). In distributed mode, the
service instances need to be built with the plugin as well.

**Runtime configuration:** cuObject reads a JSON config file (point `CUFILE_ENV_PATH_JSON` at
it) with the client NIC(s) and the RDMA properties. Without it, cuFile finds no RDMA device and
the run fails with "RDMA is not available (cuObject initialization failed)". Minimal example:

```json
{
  "properties": {
    "rdma_peer_type": "dmabuf",
    "rdma_dev_addr_list": ["<client-nic-ipv4>"],
    "rdma_multipath_enabled": true
  }
}
```

```bash
CUFILE_ENV_PATH_JSON=/path/to/cuobj.json \
  elbencho --s3endpoints http://S3SERVER -c credentials.elb --plugins s3rdma_cloudian \
    -w -r -t 4 -s 128m -b 8m mybucket
```

`rdma_peer_type: "dmabuf"` is needed on hosts without the `nvidia_peermem` module (e.g. with
the open kernel module). The further runtime notes in the `s3rdma_minio` README (multi-NIC,
`libcufile_rdma.so`, matching RoCE MTUs) apply here as well.

**Patch of the fork:** as published, the fork initializes cuObject in a static initializer, i.e.
in every process of an executable built with this plugin, also for `--version` and for runs that
do not use S3. That costs about a second per process start, writes errors to the cuFile log when
no cuObject config is present, and aborts the process (an assertion in cuFile) when the GPU has
no free memory for another CUDA context, e.g. on a host shared with other GPU jobs. The externals
build therefore applies `patches/aws-sdk-cpp-lazy-rdma-init.patch`, which defers that
initialization to the first S3 client; apply it by hand when building the fork yourself. With
the patch, a run that uses RDMA still needs free GPU memory for its CUDA context, and the
cuObject check happens when the S3 client is created, i.e. with the S3 server already reachable.

## Testing without cuObject hardware

The fork loads NVIDIA's `libcuobjclient.so` at run time, so the plugin can be tested with the
drop-in replacement for that library from [tools/s3rdma](../../../tools/s3rdma/README.md)
(`cuobjclient-shim`: the cuObject client API over Reliable Connections on plain `libibverbs`,
host memory only) against the S3-over-RDMA test server there. That runs on any RoCE NIC without a
GPU and is what `tests/run-tests.sh -r` does where NVIDIA's library cannot work. By hand:

```bash
tools/s3rdma/server/bin/s3rdma-server --dir /tmp/s3data --rdma-addr 192.168.100.10 --no-cuobj &

S3RDMA_RC_ENDPOINT=http://127.0.0.1:9000 S3RDMA_RC_ADDR=192.168.100.10 \
LD_LIBRARY_PATH=tools/s3rdma/cuobjclient-shim/lib \
  bin/elbencho --s3endpoints http://127.0.0.1:9000 --s3key k --s3secret s \
    --plugins s3rdma_cloudian -d -w -r -t 4 -s 20m -b 5m s3://mybucket
```

The fork initializes cuObject when the executable starts, so the server has to be running before
elbencho (or an elbencho service instance) starts. Note that the fork sends host memory transfers
below 1 MiB via TCP (`S3RDMA_THRESHOLD_BYTES`).

## Files

* `source/S3RdmaCloudianPlugin.*`: the plugin registration, the `--cuobjhostbufreg` option and
  the argument checks.
* `source/S3RdmaCloudianTransport.h`: the `S3Transport` that issues the fork's RDMA request
  variants with `RdmaPtr` buffers.
* `plugin.mk`: selects the fork as AWS SDK for the externals step or checks a pre-built one.
* `patches/`: the patch for the fork that the externals step applies to a fresh clone.
