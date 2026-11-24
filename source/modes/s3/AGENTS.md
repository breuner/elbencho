# S3 mode: developer notes

Design notes for the code in this directory. The conventions in the repo root
[AGENTS.md](../../../AGENTS.md) apply here as well; the plugin mechanism that builds on this
mode is described in [contrib/plugins/README.md](../../../contrib/plugins/README.md). Everything
here is built only with `S3_SUPPORT=1`; `S3_AWSCRT=1` switches the SDK client to the AWS Common
Runtime, which the `INCLUDE_AWS_S3()` macro in `source/Common.h` hides (it selects the `aws/s3/`
or `aws/s3-crt/` headers).

## Files

- `S3ProgArgs`: the S3 options (definition, defaults, checks, help, service transfer) and the
  shared S3 client singleton (`--s3single`); owned by `ProgArgs` as `s3Args`, which calls its
  methods at the matching points of its own initialization sequence.
- `S3Mode`: the worker-side code, a friend member of `LocalWorker` (`LocalWorker::s3Mode`) that
  works directly on its buffers, offset generator and statistics; dispatched per phase from
  `LocalWorker::run()`. `init()` creates or fetches the thread's S3 client, `uninit()` releases
  it, and the `iterate*()` functions are the phase loops over buckets and objects (bucket ops,
  single-part and multipart uploads incl. the async and shared variants, downloads, stat, delete,
  listing, ACLs, tagging, versioning).
- `S3Transport.h`: the compile-time seam for how object data moves between the I/O buffers and
  the server. `S3DefaultTransport` uses the HTTP body of the SDK requests; a plugin replaces the
  class via the `ELB_S3_TRANSPORT_HEADER` define (only one plugin per build), typically derived
  from the default. `S3Mode` owns one instance per worker thread and calls it directly, so there
  is no runtime dispatch. `isGpuDirect()` tells `LocalWorker` to skip the host/GPU copies.
- `S3UploadStore`: the state of multipart uploads that several workers share (`--s3mpusharing`
  from multiple clients), to know when to send the completion request and which uploads to abort
  on interruption. Process-wide singleton, reset by `S3Mode::resetSingletons()` from `ProgArgs`.
- `toolkits/S3Tk`: SDK init and client creation (see below), the in-memory `S3MemoryStream` for
  uploads/downloads without copies, checksum calculation for the stream-based requests the SDK
  does not checksum itself, the custom tree scan and the multipart upload ID precreation.
- `toolkits/S3CredentialStore`: thread-safe singleton holding several credential pairs (from a
  file or a list) for multi-user benchmarking, handed out round-robin by worker rank.
- `toolkits/S3AclTk`: translation of the user-defined ACL grantee and permissions (or a canned ACL
  name) into bucket and object requests.
- `toolkits/S3InterruptibleRetryStrategy`: decorator around the SDK's retry strategy that stops
  retries when the worker is interrupted, and marks HTTP 429/509 and the throttle exception names
  the SDK leaves non-retryable as retryable, so the SDK's own attempt limit and backoff apply.
- `toolkits/S3UnbufferedLogSystem`: SDK logger that writes every line immediately, because the
  SDK's default logger buffered lines and lost the last ones of a phase in long-running services.

## SDK and client lifetime

`S3Tk::initS3Global()` initializes the AWS SDK once per process, before any threads run
(it calls `curl_global_init()`), and `uninitS3Global()` can only be called once at the end:
the SDK does not reset its init flag in `ShutdownAPI()`, so the SDK cannot be initialized again
afterwards. A long-running service therefore keeps the SDK initialized across runs.

By default every worker thread creates its own S3 client in `S3Mode::init()` with the
interruption flag and the credentials for its rank; with `--s3single` all threads share the
client that `S3ProgArgs::prepareClientSingleton()` created. Per-run state (clients, upload store,
credential store contents) is set up and torn down per run, because a service gets new options
with every run.

## Tests

`tests/tests_s3/` runs against a libreFS server that the test library downloads on demand
(`tests/run-tests.sh -s`); the S3-over-RDMA plugins are covered by `tests/tests_plugin_s3rdma/`
(`-r`). See [tests/README.md](../../../tests/README.md).
