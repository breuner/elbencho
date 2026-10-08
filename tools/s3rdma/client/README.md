# s3rdma-client: minimal S3-over-RDMA test client

A small client for `tools/s3rdma/server` that speaks the S3-over-RDMA protocol of NVIDIA cuObject
the way elbencho's `s3rdma_*` plugins do: one body-less HTTP request per object with the
`x-amz-rdma-token` header, the object data moving over RDMA from and to host memory. It exists to
test the server independently of elbencho, and to exercise the cuObject client API with a
different library behind it.

It is a separate little project with its own `Makefile`, sharing only `tools/s3rdma/common` with
the other S3-over-RDMA test tools.

## Building

```bash
make -C tools/s3rdma/client           # output: tools/s3rdma/client/bin/s3rdma-client
make -C tools/s3rdma/client help      # build options
```

By default the client is built against the RC shim in `tools/s3rdma/cuobjclient-shim` (built first),
which works on any RoCE NIC without a GPU. To build it against NVIDIA's cuObject client library
instead (DC transport, needs the CUDA toolkit with `libcuobjclient` and a NIC with DC support):

```bash
make -C tools/s3rdma/client \
    CUOBJ_INCLUDE_PATH=/usr/local/cuda/targets/x86_64-linux/include \
    CUOBJ_LIB_PATH=/usr/local/cuda/targets/x86_64-linux/lib CUOBJ_EXTRA_LIBS=-lcufile
```

cpp-httplib is downloaded into `tools/s3rdma/common/external/` on the first build.

## Running

With the shim, the server endpoint and the local RDMA NIC are passed through the shim's
environment variables (see `tools/s3rdma/cuobjclient-shim/README.md`):

```bash
tools/s3rdma/server/bin/s3rdma-server --dir /tmp/s3data --rdma-addr 192.168.100.10 --no-cuobj &

export S3RDMA_RC_ENDPOINT=http://127.0.0.1:9000 S3RDMA_RC_ADDR=192.168.100.10
tools/s3rdma/client/bin/s3rdma-client --endpoint $S3RDMA_RC_ENDPOINT --bucket mybucket \
    --mkbucket --put --get --verify --size 4m --count 3
tools/s3rdma/client/bin/s3rdma-client --endpoint $S3RDMA_RC_ENDPOINT --bucket mybucket \
    --key obj --get --verify --size 64k --offset 1m --count 3     # ranged reads
tools/s3rdma/client/bin/s3rdma-client --endpoint $S3RDMA_RC_ENDPOINT --bucket mybucket \
    --delete --rmbucket --count 3
```

With NVIDIA's library, cuObject reads its configuration from `CUFILE_ENV_PATH_JSON` as usual and
the variables above are not needed.

`--help` lists all options. Objects are written with a data pattern that depends on the offset
and the object index, which `--verify` checks on downloads, including ranged ones. Requests are
not signed, so the client only works with servers that do not check signatures.

## Files

* `source/Main.cpp`: command line and the operation sequence.
* `source/RdmaS3Client.*`: the protocol: token minting through the cuObject API, the control
  requests via cpp-httplib, and the reply handling as in elbencho's `s3rdma_minio` plugin.
