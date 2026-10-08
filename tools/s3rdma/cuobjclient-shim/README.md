# cuobjclient-shim: the cuObject client API over Reliable Connections

A drop-in replacement for NVIDIA's cuObject client library (`libcuobjclient`, `cuobjclient.h`)
for testing S3-over-RDMA code on hosts without the hardware that cuObject needs. It offers the
same types, class and method signatures, with identical mangled symbols and class layout, but
moves the data over Reliable Connections (RC) on plain `libibverbs` instead of cuObject's DC
transport, from and to host memory. It works on any RoCE NIC, also where cuObject's DC queue
pairs cannot be created, and needs no GPU and no CUDA toolkit.

The intended use is to run programs written for cuObject, such as elbencho's `s3rdma_*` plugins
or `tools/s3rdma/client`, unchanged against `tools/s3rdma/server`.

## How it works

cuObject's protocol is one object request per transfer: the client mints a token for its buffer
(`cuMemObjGetRDMAToken`), sends it in the `x-amz-rdma-token` header, and the server RDMA-reads or
-writes the buffer while it handles the request. An RC queue pair can only do that when it is
connected to the peer's queue pair already, so the shim connects when the token is minted:

1. `cuMemObjGetRDMAToken()` creates an RC queue pair and sends its number, GID and packet
   sequence number to the S3 server's `POST /.s3rdma-rc/connect` endpoint, in the token format
   of AMD hipObject. The server creates and connects its own queue pair and answers with a
   connection id, its token and sequence number; the shim connects its queue pair to it.
2. The minted token names that connection: `rc:<connection id>:<address>:<size>:<rkey>`. The
   application sends it with the object request exactly like a cuObject token, and the server's
   RC transport does the RDMA transfer through the connected queue pair.
3. `cuMemObjPutRDMAToken()` tells the server to drop the connection (`POST /.s3rdma-rc/disconnect`)
   and destroys the queue pair.

This connect step is an extension of `tools/s3rdma/server`; the shim does not work with other S3
servers. Buffers registered via `cuMemObjGetDescriptor()` are pinned with `ibv_reg_mr()`. The
RDMA device and the registrations are shared by all `cuObjClient` instances of a process, as with
NVIDIA's library: a buffer registered through one instance can be used for tokens of another one
(the Cloudian SDK fork does that with a thread-local instance per thread).

## Configuration

cuObject's API has no parameters for the server or the NIC, so the shim reads them from the
environment:

| Variable | Meaning |
| :--- | :--- |
| `S3RDMA_RC_ENDPOINT` | `http://host:port` of the S3 server (required) |
| `S3RDMA_RC_ADDR` | IPv4 address of the local RDMA NIC to use (required) |
| `S3RDMA_RC_GID_INDEX` | GID table index to use; default is the RoCE v2 GID of the address |

`cuObjClient::isConnected()` returns false and an error is printed when the variables are missing
or the NIC cannot be opened, like cuObject behaves when its fabric is unavailable.

## Building and using

```bash
make -C tools/s3rdma/cuobjclient-shim    # output: lib/libcuobjclient.so.1 and the .so link
```

Needs `libibverbs` development files (Linux only). cpp-httplib is downloaded into
`tools/s3rdma/common/external/` on the first build.

To build a cuObject program against the shim, point it at `include/` and `lib/` instead of the
CUDA toolkit dirs; for `tools/s3rdma/client` that is the default, and elbencho's `s3rdma_minio`
plugin does it with `make ... ELB_PLUGIN_S3RDMA_MINIO=1 CUOBJ_SHIM=1`. Since the library has the
soname `libcuobjclient.so.1` and the symbols of NVIDIA's library, a program that was linked
against NVIDIA's `libcuobjclient` can also pick up the shim at run time with
`LD_LIBRARY_PATH=tools/s3rdma/cuobjclient-shim/lib`; the same works for programs that load the
library with `dlopen()`, like the Cloudian SDK fork behind elbencho's `s3rdma_cloudian` plugin.

## Limits

* Host memory only; `getMemoryType()` always reports system memory.
* Implemented: constructor/destructor, `isConnected`, `cuMemObjGetDescriptor`,
  `cuMemObjPutDescriptor`, `cuMemObjGetMaxRequestCallbackSize`, `cuMemObjGetRDMAToken`,
  `cuMemObjPutRDMAToken` (both overloads), `cuObjGet`/`cuObjPut` (through the callbacks),
  `getCtx`, `getMemoryType`; the telemetry functions are no-ops. External RC connections and
  `getRDMAPDList` are not part of the header.
* One connect round trip per token. That costs a few hundred microseconds per transfer, fine for
  functional tests, not representative for performance.
