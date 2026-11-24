# Netbench mode: developer notes

Design notes for the code in this directory. The user-level description of the mode, the
engines and the placement rules is in [docs/netbench.md](../../../docs/netbench.md); the
conventions in the repo root [AGENTS.md](../../../AGENTS.md) apply here as well.

## Files

- `NetBenchProgArgs`: the netbench options (definition, defaults, checks, `--help-netbench`,
  service transfer); owned by `ProgArgs` as `netBenchArgs`.
- `NetBenchMode`: the worker-side code, a friend member of `LocalWorker` that works directly on
  its buffers and stats and is dispatched per phase from `LocalWorker::run()`.
- `NetBenchEngine.h`: the engine abstraction behind which the data path sits: the abstract
  `NetBenchEndpoint`, the per-stream `NetBenchConn`, `NetBenchOpCtx`, `NetBenchCompletion`, the
  IP/interface helpers and the `create()` factory, selected per run via `--netengine`.
- `NetBenchEngineTcp`: one non-blocking socket per stream; the client's ping doubles as hello
  with the rank. Always built.
- `NetBenchEngineIbverbsBase`: the data path shared by the two RDMA engines (RC queue pairs on
  libibverbs, one completion channel per endpoint).
- `NetBenchEngineIbverbs` (`IBVERBS_SUPPORT`): one RC queue pair per stream, connected by
  exchanging the queue pair details over HTTP, no rdma_cm.
- `NetBenchEngineRdmaCm` (`RDMACM_SUPPORT`): the same queue pairs connected by RDMA CM, so with
  ECE; the HTTP reply carries the listen address, the server's queue pair is created on the
  connect request event, receives posted before that are queued.
- `NetBenchEngineLibfabric` (`LIBFABRIC_SUPPORT`): reliable datagram endpoint with tagged
  messages.
- `NetBenchProtocol.h`: the connect JSON keys, the libfabric tag layout and the timeouts.
- `NetBenchPlacement`: assigns every client stream to a server interface (see below).
- `NetBenchServer`: the process-wide registry of a server's endpoints (see below).

## Run setup

Before a run, the coordinator asks every service for its current interfaces
(`/netbenchifaces`, implemented in both HTTP backends); the `--netdevs` entries are resolved per
run. The reply also carries a per-process service ID, by which further addresses of an already
listed service are dropped as aliases, and the hostname, shown after numeric service addresses in
messages. `NetBenchPlacement` then assigns every stream to a server interface in the same subnet
as the client interface (`--netcrosssubnet` relaxes this, `--netdevsubnets` takes the subnets
from the `--netdevs` CIDR entries instead of the netmasks), balanced over servers first, then
interfaces; each host's part of the result travels in the prepare tree.

Each client thread runs one stream to one server. A server registers its endpoints in the
process-wide `NetBenchServer`, through which the HTTP connect handler (`/netbenchconnect`, both
HTTP backends) adds client streams (and posts their receives) before the phase starts, and which
drives engine progress (tcp accepts, libfabric provider progress) until the worker threads take
over.

## Data path

Completion waiting is event-based (wait fds, `poll()`), not busy polling. Every endpoint owns
one message buffer slab, which on a server has a receive slot for every block its clients can
have in flight, so there is no receiver-not-ready in ping-pong streams; one-directional streams
scale the receiver's slots by `--netrecvpct`. Old libfabric versions crash when an endpoint with
queued unexpected messages is closed, which is why the slot count matters there too.

The tcp engine is always built, the other engines sit under their `*_SUPPORT` defines and must
degrade to a clear error when compiled out. As in every mode, nothing per I/O may be decided in
the loop: engine, block sizes and buffers are set up once per phase.
