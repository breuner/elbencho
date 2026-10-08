# S3-over-RDMA test tools

Small, self-contained tools for testing elbencho's S3-over-RDMA plugins (`contrib/plugins/`)
without a real RDMA-capable object store. They are separate little projects with their own
Makefiles, independent of the main elbencho build, and are used by the test suite
(`tests/tests_plugin_s3rdma/`, `tests/run-tests.sh -r`).

| Directory | Contents |
| :--- | :--- |
| [server/](server/README.md) | `s3rdma-server`: a minimal S3 server that moves object data over RDMA. Speaks NVIDIA cuObject's protocol (DC transport via `libcuobjserver`, or RC transport for the shim below) and AMD hipObject's `hipobj-rc-v2` protocol, and stores objects as plain files. |
| [client/](client/README.md) | `s3rdma-client`: a minimal client for the server that speaks cuObject's protocol through the cuObject client API, like the elbencho plugins do. |
| [cuobjclient-shim/](cuobjclient-shim/README.md) | `libcuobjclient.so`: a drop-in replacement for NVIDIA's cuObject client library that moves data over Reliable Connections on plain `libibverbs`, for hosts without DC-capable NICs or GPUs. |
| [common/](common/README.md) | Code shared by the three tools. |

Typical use on a host with a RoCE NIC but no GPU, with the test client and with elbencho's
`s3rdma_minio` plugin (the shim replaces NVIDIA's library at run time; the `s3rdma_cloudian`
plugin works the same way):

```bash
make -C tools/s3rdma            # builds all three (or make -C tools/s3rdma/<tool> for one)

tools/s3rdma/server/bin/s3rdma-server --dir /tmp/s3data --rdma-addr 192.168.100.10 --no-cuobj &

export S3RDMA_RC_ENDPOINT=http://127.0.0.1:9000 S3RDMA_RC_ADDR=192.168.100.10
tools/s3rdma/client/bin/s3rdma-client --endpoint $S3RDMA_RC_ENDPOINT --bucket mybucket \
    --mkbucket --put --get --verify --size 4m --count 3

LD_LIBRARY_PATH=tools/s3rdma/cuobjclient-shim/lib bin/elbencho --s3endpoints $S3RDMA_RC_ENDPOINT \
    --s3key k --s3secret s --plugins s3rdma_minio -d -w -r -t 4 -s 4m -b 4m s3://mybucket
```

`tests/run-tests.sh -r` builds the tools and runs the plugin tests with them, see
[tests/README.md](../../tests/README.md). `make -C tools/s3rdma clean` and `clean-all` are
forwarded to all tools (the main elbencho `make clean-all` forwards here too).
