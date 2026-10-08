# s3rdma-server: minimal S3-over-RDMA server for testing elbencho

A small S3 server that speaks the S3-over-RDMA protocols of NVIDIA cuObject (the
`x-amz-rdma-*` headers; DC transport, or RC transport with the client shim in
`tools/s3rdma/cuobjclient-shim`) and AMD hipObject (`hipobj-rc-v2`, RC transport), and stores
objects as plain files in a directory. It exists to test elbencho's `s3rdma_*` plugins (see
`contrib/plugins/`) without a real RDMA-capable object store: the test suite starts a private
instance per test script (`tests/tests_plugin_s3rdma/`), and it can be started by hand for
interactive testing. It is deliberately not a complete or secure S3 implementation.

It is a separate little project with its own `Makefile`, shares no code with elbencho and is not
part of any release build or package. Code shared with the other tools in `tools/s3rdma/` lives
in `tools/s3rdma/common`.

## Building

```bash
make -C tools/s3rdma/server            # output: tools/s3rdma/server/bin/s3rdma-server
make -C tools/s3rdma/server help       # build options
```

Requirements:

* A C++17 compiler and `make`.
* [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT), a single header that the
  `Makefile` downloads into `tools/s3rdma/common/external/` on the first build
  (`HTTPLIB_VERSION` pins the release).
* For cuObject's DC transport: NVIDIA's cuObject server library (`libcuobjserver` with
  `cuobjserver.h`, from the CUDA apt/dnf repositories; it is NVIDIA proprietary and not shipped
  with elbencho) plus `libibverbs` and `librdmacm`. Auto-detected (`CUOBJ_SUPPORT`,
  `CUOBJ_INCLUDE_PATH`, `CUOBJ_LIB_PATH` override this).
* For the RC transports (client shim and hipObject protocol): only `libibverbs` (auto-detected,
  `RC_SUPPORT`). Nothing of hipObject itself is needed by the server. hipObject's own RDMA test
  client gets built from the hipObject sources for the test suite when `libibverbs` and OpenSSL
  development files are present (`HIPOBJ_CLIENT`, `HIPOBJ_COMMIT`); the sources are downloaded
  into `build/` on the first build.

Without either library, the server still builds as a plain HTTP S3 server that declines RDMA
requests (`--no-rdma`), also on macOS.

No transport needs a GPU or a GPU toolkit; the RDMA transfers go to and from host memory. The
cuObject transport does need a NIC that supports DC (Dynamically Connected queue pairs), i.e.
NVIDIA ConnectX-5 or newer; on older NICs the RDMA session cannot be opened, which `--rdma-check`
reports. The RC transports work on any RoCE NIC.

## Running

```bash
# S3 endpoint on 127.0.0.1:9000, RDMA transfers via the NIC that has this IP address:
tools/s3rdma/server/bin/s3rdma-server --dir /tmp/s3data --rdma-addr 192.168.100.10 --verbose

# plain HTTP S3 server, RDMA requests are declined:
tools/s3rdma/server/bin/s3rdma-server --dir /tmp/s3data --no-rdma

# only check whether the RDMA transports can be set up (exit code 0 = yes):
tools/s3rdma/server/bin/s3rdma-server --rdma-check --rdma-addr 192.168.100.10

# the same for one transport only, e.g. without cuObject:
tools/s3rdma/server/bin/s3rdma-server --rdma-check --rdma-addr 192.168.100.10 --no-cuobj
```

`--help` lists all options: HTTP address and port, cuObject port, staging buffer size, thread
count, GID index for the RC transport, `--no-cuobj` / `--no-rc` to disable one transport. Any
access key and secret are accepted, signatures are not verified.

elbencho with the `s3rdma_minio` plugin and NVIDIA's cuObject client library, with a cuObject
client config that names the client NIC (see `contrib/plugins/s3rdma_minio/README.md`):

```bash
CUFILE_ENV_PATH_JSON=/path/to/cuobj.json bin/elbencho --s3endpoints http://127.0.0.1:9000 \
    --s3key k --s3secret s --plugins s3rdma_minio -d -w -r -t 4 -s 4m -b 4m s3://mybucket
```

On a host without DC support, the same protocol runs over RC with
[tools/s3rdma/client](../client/README.md) or any other cuObject program built against the
[client shim](../cuobjclient-shim/README.md). hipObject transfers can be tried with the test
client from the hipObject repository that gets built next to the server (it uses fixed test
credentials, which this server does not check anyway):

```bash
tools/s3rdma/server/bin/hipobj-v2-data-client 127.0.0.1 9000 PUT /mybucket/mykey 64
tools/s3rdma/server/bin/hipobj-v2-data-client 127.0.0.1 9000 GET /mybucket/mykey 64
```

With `--verbose`, the server logs every request and one line per RDMA transfer
(`RDMA PUT <bucket>/<key>: <bytes> bytes`), which is how the tests confirm that the data really
moved over RDMA.

## What it implements

S3 API subset (path-style addressing only): CreateBucket, HeadBucket, DeleteBucket, ListBuckets,
ListObjects(V2) (everything in one page, no pagination or URL encoding of keys), PutObject,
GetObject (with `Range`), HeadObject, DeleteObject, DeleteObjects, CreateMultipartUpload,
UploadPart, CompleteMultipartUpload (concatenates all uploaded parts, the part list of the request
is not validated), AbortMultipartUpload. Everything else (ACLs, tagging, versioning, object lock,
...) is answered with `501 NotImplemented`. ETags are opaque values derived from the file, not
MD5 sums.

### NVIDIA cuObject protocol

This is what the `s3rdma_minio` plugin and Cloudian's SDK fork (`s3rdma_cloudian`) send:

* A request with an `x-amz-rdma-token` header has an empty body. The token is cuObject's RDMA
  descriptor; the server passes it unchanged to `libcuobjserver` and only reads the client buffer
  address and size from its first two fields.
* PutObject/UploadPart: the server RDMA-reads the object data from the client buffer (the size
  comes from the token, not from a header) and answers `200` with `ETag`, `x-amz-rdma-reply: 200`
  and `x-amz-rdma-bytes-transferred`.
* GetObject: the server RDMA-writes the requested bytes into the client buffer and answers with
  an empty body, `x-amz-rdma-reply: 200` (whole object) or `206` (with `Content-Range`, for a
  `Range` request) and `x-amz-rdma-bytes-transferred`.
* `x-amz-rdma-reply: 501` declines an RDMA request, e.g. with `--no-rdma`.

Transfers larger than the staging buffer size (`--bufsize`, default 16 MiB, one buffer per
thread) are done in chunks, so the object size is not limited by pinned memory.

### cuObject protocol over RC (the client shim)

The same object requests work over Reliable Connections with
[tools/s3rdma/cuobjclient-shim](../cuobjclient-shim/README.md), a drop-in replacement for
NVIDIA's client library. An RC queue pair has to be connected before the server can RDMA-read or
-write the client buffer, so the shim connects when it mints a token: `POST /.s3rdma-rc/connect`
carries the client's queue pair (in hipObject's token format) and packet sequence number, the
server connects a queue pair of its own and answers with a connection id, its token and sequence
number (`x-amz-rdma-session`, `x-amz-rdma-reply: 200:<token>`, `x-amz-rdma-psn`). The object
request then carries `x-amz-rdma-token: rc:<connection id>:<address>:<size>:<rkey>`, which the
server's RC transport serves through that connection, chunked via the same staging buffers as
above. `POST /.s3rdma-rc/disconnect` drops the connection; idle connections expire after five
minutes. This connect step is an extension of this server, not part of cuObject's protocol.

### AMD hipObject protocol (`hipobj-rc-v2`)

[hipObject](https://github.com/ROCm/hipObject) is a client library for AMD GPUs. It uses the same
`x-amz-rdma-*` headers as cuObject, but a Reliable Connection (RC) transport, which needs both
sides to know each other's queue pair before any data can move. Its `hipobj-rc-v2` control
protocol therefore takes two round trips, both body-less `POST`s with `x-amz-rdma-*` headers to
`/.hipobj-rc/prepare` and `/.hipobj-rc/ready` (plus `/.hipobj-rc/cancel`):

* PREPARE names the operation, the target (`/bucket/key`), the size and offset, the client's
  88-hex RC token (queue pair, GID) and its packet sequence number. The server creates a session
  with its own queue pair and a registered staging buffer (for a GET already filled with the
  object data) and answers with the session id, its queue pair number, sequence number and, in
  `x-amz-rdma-reply: 200:<token>` plus `x-amz-rdma-mr-*`, the staging buffer's address and key.
* READY connects the two queue pairs. For a PUT the client RDMA-writes the data into the staging
  buffer with the session cookie as immediate value, which the server receives, verifies and
  stores; for a GET the server RDMA-writes the object data into the client's buffer the same
  way. The response echoes the cookie and reports the bytes transferred.

The server matches its source GID to the peer's: RoCE v2 for IPv4-mapped peer GIDs, RoCE v1 for
link-local ones, or a fixed index via `--rdma-gid-index`. Control requests without the protocol
header, and object requests that carry a hipObject RC token directly (hipObject's single round
trip variant, which cannot connect the queue pairs in time), are answered with `501` and
`X-Amz-Rdma-Protocol-Status: unsupported`, the protocol's marker for a server that does not speak
that variant.

## Layout

* `source/Main.cpp`: command line and wiring.
* `source/S3Server.*`: the HTTP side (cpp-httplib), S3 request dispatching, the RDMA control
  requests and the choice of transport by token format.
* `source/ObjectStore.*`: buckets as dirs, objects as files, multipart uploads below
  `<bucket>/.mpu/`.
* `source/RdmaTransport.h`: the two-function interface for moving data to/from a client buffer
  in the cuObject protocol; `source/CuObjTransport.*`: its `libcuobjserver` implementation
  (compiled only with `CUOBJ_SUPPORT=1`); `source/RcTokenTransport.*`: its RC implementation
  for the client shim's tokens, with the connect endpoints.
* `source/HipObjV2Server.*`: the `hipobj-rc-v2` control endpoints and sessions.
* `source/RdmaToken.h`, `source/S3Xml.h`: token parsing and the few S3 XML documents.
* The RC code uses `RcDevice.*` and `HipObjToken.h` from `tools/s3rdma/common` and is compiled
  only with `RC_SUPPORT=1`.
