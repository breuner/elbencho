# common: code shared by the S3-over-RDMA test tools

Sources used by more than one of `tools/s3rdma/server`, `tools/s3rdma/client` and
`tools/s3rdma/cuobjclient-shim`. Nothing here is built on its own; each tool compiles what it
needs from this directory with its own Makefile.

* `RcDevice.*`: RDMA device, Reliable Connection queue pairs and transfers on plain `libibverbs`.
* `HipObjToken.h`: the 44-byte RC token format of AMD hipObject, used for the queue pair
  handshakes.
* `HexTk.h`: number formatting and parsing for the `x-amz-rdma-*` headers.
* `HttpLib.h`: the one include of cpp-httplib, with the settings all tools share.
* `Log.h`: minimal logging.
* `httplib.mk`: shared download rule for cpp-httplib; the header lands in `external/` here.
