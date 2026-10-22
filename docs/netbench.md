# Network Benchmarking

elbencho's netbench mode measures the network between hosts the way a storage benchmark would
use it: clients send blocks to servers and wait for their response, like write requests to a
storage server and their acknowledgements, or like read requests and their data. The transport
is one of three engines, selected with `--netengine`: plain TCP sockets (the default), InfiniBand
or RoCE through libibverbs, or [libfabric](https://github.com/ofiwg/libfabric) with any of its
providers (TCP, verbs, AWS EFA, HPE Slingshot, ...). The test itself is the same for all of them.

## Setup

Every host that takes part runs an elbencho service, servers and clients alike:

```bash
elbencho --service
```

The benchmark is then started from any host (it does not need to be one of the services) by
naming the servers and the clients:

```bash
elbencho --netbench --servers server[1-2] --clients client[1-4] -t 8 --timelimit 30
```

Each client thread (`-t` per client host) runs one stream to one server. Per round, the client
sends `--sendsize` bytes (the netbench name of `-b`, default 1 MiB) and the server answers with
`--recvsize` bytes (default 1). `-s` is the number of bytes that each client thread sends in
total; it defaults to the largest value whose total over all streams still fits the 64 bit
counters (petabytes per stream), so runs are normally ended by `--timelimit`.

`-t` is the number of threads of every service. Servers and clients can use fewer of them with
`--tservers` and `--tclients`, e.g. `--tservers 8 --tclients 1` for one server with 8 threads
and many clients with a single stream each; the remaining threads of a service stay idle. If
`-t` is not given, it is implicitly the larger of the two.

| Scenario | Options |
| :--- | :--- |
| Writes with 1 MiB blocks | `--sendsize 1m --recvsize 1` |
| 4 KiB reads, 16 requests in flight per stream | `--sendsize 1 --recvsize 4k --iodepth 16` |
| Pure bandwidth test, client to server only | `--sendsize 4m --recvsize 0` |
| Pure bandwidth test, server to client only | `--sendsize 0 --recvsize 4m` |
| Fixed amount instead of a time limit | `-s 10g` |
| 8 server threads, 1 stream per client | `--tservers 8 --tclients 1` |

`--iodepth` is the number of rounds in flight per stream. The default of 1 is a ping-pong, where
the reported latency is the round trip time of one block and its response.

## Engines

`--netengine tcp|ibverbs|rdmacm|libfabric` selects the transport on the coordinator; all services
of a run use it, and a service can run a different engine in every run without being restarted. The
connection setup is the same for all engines: a client registers its stream at its server
through the service HTTP API and gets the server's endpoint details back, then it verifies the
data path with a small ping message. Streams of all engines wait for completions through file
descriptors (`poll()`), there is no busy polling.

| Engine | Transport | Needs | File descriptors on a server |
| :--- | :--- | :--- | :--- |
| `tcp` (default) | one TCP socket per stream | nothing | 1 per stream (plus 1 listener per endpoint until the phase starts) |
| `ibverbs` | one InfiniBand/RoCE reliable connection (RC queue pair) per stream | libibverbs (rdma-core 30 or newer) | 1 per endpoint (thread x interface), plus 2 per RDMA device |
| `rdmacm` | the same connections, set up by RDMA CM (ECE negotiation) | libibverbs and librdmacm (rdma-core 30 or newer) | 2 per endpoint, plus librdmacm's device contexts |
| `libfabric` | libfabric reliable datagram endpoint, provider via `--netprov` | libfabric 1.11 or newer | about 10 per endpoint, plus 1 per stream for the tcp provider |

### tcp

Plain non-blocking TCP sockets with `TCP_NODELAY`, IPv4 and IPv6. Both sides know the message
sizes, so the byte stream needs no framing. A server listens on one socket per endpoint, which
the clients learn through the service API (or `--netport`), and closes it when the phase starts.

### ibverbs

InfiniBand and RoCE (v2 preferred over v1) straight on libibverbs: one RC queue pair per stream
with the queue pair numbers, GIDs and sequence numbers exchanged through the service API, so
rdma_cm is not involved at all and neither is its ECE negotiation of transport options (use the
`rdmacm` engine to measure with ECE). An IP address
selects the RoCE GID that carries it, or the InfiniBand port behind an IPoIB interface; `--netdevs`
also takes RDMA device names (`mlx5_0`). Blocks go via RDMA send/receive; a server posts a
receive for every block its clients can have in flight (see Memory below), so receiver-not-ready
retries are rare (`--netrnr` sets their timer). `--netport` has no meaning for this engine. The pinned memory is
just elbencho's own buffers plus the queues, so the locked memory limit (`ulimit -l`) has to
cover (client threads per server) x `--iodepth` x `--sendsize` on a server and `--iodepth` x
(`--sendsize` + 2 x `--recvsize`) per client thread.

### rdmacm

The same reliable connections as the ibverbs engine (same data path, same memory needs), but set
up by RDMA CM through librdmacm: a server listens on its interface address (`--netport` fixes the
port), the service API hands the client only that address, and the client resolves and connects
to it like any rdma_cm application. So the adapters negotiate the connection parameters among
themselves, including ECE (Enhanced Connection Establishment), which enables vendor features
such as the congestion control mode of Zero Touch RoCE. That makes this engine the one to
compare against `ibverbs` when such features are in question: identical traffic, with and
without ECE. With `--log 1` on the clients, each connection logs the negotiated ECE vendor and
options (or "no ECE" where an adapter does not support it). `--netdevs` entries have to be
addresses, subnets or interface names for this engine, not RDMA device names. Each endpoint
costs two file descriptors (completion channel and connection event channel); the device
contexts are librdmacm's and shared by the process.

### libfabric

`--netprov <name>` names the libfabric provider (default tcp) and implies this engine;
`--netprov rdma` takes the first available RDMA provider (verbs for InfiniBand and RoCE, efa,
cxi, ...). All hosts of a run have to use the same provider, and ideally the same libfabric
version: the same provider name does not guarantee the same wire protocol across versions (the
tcp provider was rewritten in libfabric 1.18 and does not interoperate with older versions,
while verbs worked across 1.17 and 2.7 in our tests). A server logs a note when a client connects
with a different libfabric version.

```bash
elbencho --netbench --netprov list   # lists the providers and interfaces of this host
```

libfabric itself can be tuned and debugged through its `FI_*` environment variables, e.g.
`FI_LOG_LEVEL=warn` for provider warnings. Note that the verbs provider moves blocks of 128 KiB
and more via RDMA reads instead of send/receive. A recent libfabric (2.7 or newer, which
`make BUILD_LIBFABRIC=1` builds) is recommended: older versions were seen crashing while
closing endpoints at the end of interrupted runs with many streams, which elbencho works around
by draining its endpoints before closing them.

## Memory

A server posts a receive buffer for every block that its clients can have in flight, i.e.
(client threads per server) x `--iodepth` x `--sendsize` bytes per server, spread over its
threads and interfaces, and a client one buffer per round in flight for the responses. So no
message ever has to wait inside the transport: in ping-pong streams the ibverbs and rdmacm
engines never see a receiver-not-ready condition (a side only sends again after the other side
reposted its buffer and answered), with libfabric large blocks are pulled via RDMA read as soon
as they are announced. (The `verbs;ofi_rxm` provider stack additionally has credit-based flow
control between the peers, so even the libfabric-internal receive buffers cannot overflow.)

One-directional streams have no such pacing: in a send-only stream (`--recvsize 0`) the client
sends its next block as soon as the server's adapter acknowledged the previous one, while the
server reposts the consumed buffer only after its thread saw the completion; in a receive-only
stream (`--sendsize 0`) the roles are swapped. The receiving side of such a stream therefore
posts `--netrecvpct` percent of the sender's `--iodepth` as receive buffers (default 200, i.e.
twice as many as the sender can have in flight; rounded up, at least one). A higher value
widens the margin that a slow receiver thread has before the sender has to wait, a value below
100 provokes receiver-not-ready (RNR) retries on purpose, e.g. to measure their effect. The
ibverbs and rdmacm engines retry such blocks indefinitely after the timer given by
`--netrnr <code>`, an InfiniBand RNR NAK timer code from 0 to 31 (1 = 10 us, the default; 12 =
0.64 ms; 0 = 655 ms, the kernel default of rdma_cm). The libfabric verbs provider sets its own
timer from `FI_VERBS_MIN_RNR_TIMER` when the service process first uses libfabric, so the option
is rejected for the other engines. The adapter counters show how often it happened
(`rdma statistic` or `/sys/class/infiniband/<dev>/ports/1/hw_counters/rnr_nak_retry_err`). With
the tcp engine, fewer receive buffers than blocks in flight just leave the data in the socket
buffer for a while.

So a client thread needs `--iodepth` x `--sendsize` plus its receive buffers x `--recvsize` of
buffer memory, a server (streams per server) x receive buffers per stream x `--sendsize` plus
one `--recvsize` response buffer per endpoint (thread x interface), where the receive buffers
per stream are `--iodepth` for ping-pong streams and `--netrecvpct` of it for one-directional
ones. `--netmemmax <size>` (default 1 GiB) rejects a run whose buffers would exceed that amount
on a client or on a server, for every engine, and the error message states both amounts with
their calculation, so the limit can be raised knowingly (0 means no limit). The buffers of the
RDMA engines are pinned, so this is also the memlock need; the libfabric verbs provider pins
more, see below.

RDMA engines pin memory, so the locked memory limit (`ulimit -l`) has to be generous; RDMA
users typically run with `unlimited`. The ibverbs engine pins elbencho's buffers and little
else. The libfabric verbs provider additionally pins about 65 MiB per data endpoint (one per
service thread and interface, client threads included) at libfabric's defaults, so a limit of a
few MiB, as systemd gives its services by default, makes the endpoint setup fail with "Cannot
allocate memory". Most of that memory is sized by the expected number of peers per endpoint,
`FI_UNIVERSE_SIZE` (default 256 in libfabric 1.x, 1024 in 2.x), which libfabric reads once per
process; lower it for client hosts with many threads or hosts with a limited memlock, and keep
it above the number of client threads per server endpoint. Each client stream also costs the
server about 2 MiB of libfabric-internal receive buffers with the verbs provider
(`FI_OFI_RXM_MSG_RX_SIZE` x `FI_OFI_RXM_BUFFER_SIZE`).

Each libfabric endpoint also costs about 10 file descriptors. elbencho raises its soft open
files limit to the hard limit, so the hard limit (`ulimit -Hn`) of a server service has to cover
threads x interfaces x 10, plus the limit's own baseline. The tcp, ibverbs and rdmacm engines need one or
two descriptors per stream or per endpoint (see the table above).

## Interfaces and ports

`--netdevs` is a service option: a server opens one data endpoint per thread on each listed
interface, a client binds its threads to the listed interfaces. Interfaces can be given by name
(`eth0`), by IP address (IPv4 or IPv6), by IP subnet in CIDR notation (`172.200.0.0/16` stands
for every interface that has an address in that subnet) or by RDMA device or libfabric domain
name (`mlx5_0`, `cxi0`, `rdmap0s6-rdm`). An interface name resolves to the interface's address
of the family of the service address.

```bash
elbencho --service --netdevs eth1,eth2,eth3     # on the servers
elbencho --service --netdevs eth1,eth2          # on the clients
elbencho --service --netdevs 172.200.0.0/16     # all interfaces in that subnet
```

A service refuses to start when none of its `--netdevs` entries matches an interface and prints
a warning (on the console, before it goes into the background) for each entry that matches
nothing while others do. The entries are resolved again at the start of every benchmark run, so
an interface that comes up later (a replaced cable, a new address) is used from the next run on
without a service restart, and one that went down is left out.

Without `--netdevs`, a server uses the interface of the address that the coordinator gave in
`--servers`, and a client may use one interface per subnet (the first one; the kernel routes
through the first interface of a subnet anyway, so list the interfaces explicitly to use several
in one subnet). The data endpoints use ports chosen by the OS, which the clients learn through
the service HTTP API. `--netport <port>` on a server service fixes a base port instead;
endpoint number N listens on this port plus N.

### Stream placement and subnets

Hosts with several interfaces are often in several IP subnets that are physically separate
networks, so a stream must not connect an interface in one subnet with an interface in another.
Before a run, the coordinator asks every service for its current interfaces and places each
client stream on a server interface in the same subnet as the client interface. Two interfaces
are in the same subnet when each address lies in the other's network, so a netmask that differs
between two hosts does not matter. The network of an interface is defined by its netmask; a
subnet given in `--netdevs` narrows it when it is more specific (`--netdevs
172.200.5.0/24,172.200.6.0/24` keeps two subnets apart even if both interfaces carry a /16
netmask). IPv4 and IPv6 interfaces are never paired. `--netcrosssubnet` on the coordinator
allows streams between different subnets, e.g. when the subnets are routed between each other;
same-subnet pairs are still preferred then.

The opposite case, a subnet wider than the netmask, comes up in routed (L3) networks where the
ports carry host addresses (`172.16.0.1/32` on the port in one network, `172.16.64.101/32` on
the port in the other): the netmask then says nothing about the network, every address would
be a subnet of its own and no stream could be placed. `--netdevsubnets` on the coordinator
makes the subnets given in the services' `--netdevs` lists the subnets of the placement, so
with `--netdevs 172.16.0.0/18,172.16.64.0/18` on all services the ports in the first network
are paired with each other and the ports in the second network with each other. Entries given
by interface name or address keep the subnet of their netmask.

The placement balances the streams globally under those constraints: the servers get equal
numbers of streams first (so a server does not look slow merely because it got fewer streams),
then the interfaces of each server, then the interfaces of each client. With every interface in
one subnet this is the plain round-robin placement: client thread number R (counted over all
clients) talks to server number R modulo the number of servers, and the clients of a server go
round-robin over its active worker threads (`--tservers`) and over its interfaces. With a client
that is only in one of the subnets (e.g. a broken cable), the other clients fill the servers it
cannot reach. When a group of servers or interfaces still ends up with uneven stream counts, the
coordinator prints a `NOTE` with the counts, and `--log 1` shows the interfaces every service
reported. A client that shares no subnet with any server, or a server that no client can reach,
is an error before anything is prepared (the message names the subnets of both sides). The
control connections between the coordinator and the services (and between clients and servers)
use the service addresses and are exempt from this rule.

With several RDMA interfaces in the same IP subnet, the kernel may route traffic from the
second interface's address through the first interface (`ip route get <peer> from <own address>`
shows the device), and the peer may resolve the second address to the first interface's MAC.
TCP tolerates both, RDMA connections over the additional interfaces then fail intermittently
(the connection check times out). Source-based policy routing rules (`ip rule add from <own
address> lookup <table>` plus a route for the subnet through the matching device in that table)
and `arp_ignore=1`/`arp_announce=2` make each interface answer and send only for its own
address. This applies to both RDMA engines, as the GID of a RoCE interface is selected by its
address, not by the route.

Streams progress independently of each other, so one slow interface does not throttle the other
streams of the same host. It shows up as a difference between the "First Done" and "Last Done"
result columns, and each stream logs its own throughput when it finishes (`--log 2` on the
services).

## Results

The result reports the netbench phase as `NET/<engine>` (e.g. `NET/tcp`; the console table cuts
long names to its column width), as seen by the clients: the "send" rows count
the blocks and bytes the clients sent (`clients * threads * rounds * sendsize` bytes for a
complete run), the "recv" rows the responses and bytes they received
(`clients * threads * rounds * recvsize`). The servers see the same traffic from the other side
and are therefore not added to these totals (they are shown in the live statistics and in the
`--svcminmax` rows). The latency is measured on the clients from sending a block to receiving
its response (or to the completion of the send for a send-only stream). In the json and csv
result files the send values are the main values and the recv values are the `rwmix_read` or
"rwmix read" values, as for a read/write mix.

`--svcminmax` adds the slowest and the fastest service to the result, which makes a broken cable
or a misconfigured interface stand out: one row for IOPS and one for throughput, each with the
minimum, average and maximum over the services and the host names of the two extremes, based on
each service's own completion time, on the bytes in both directions and on the rounds. Servers and
clients are not comparable (a server with ten clients moves ten times the data of each of
them), so netbench shows a "Clients" and a "Servers" row pair. The same values go into the json
result file (`--jsonfile`) as `clients` and `servers` subtrees of `last_done`. The option works
for the other benchmark modes too, where it compares all services as one group (`Svc` rows, a
`services` subtree).

## Build

Netbench mode is always built; the tcp engine needs no library.
The ibverbs engine (`IBVERBS_SUPPORT`) is built when the libibverbs development files of
rdma-core 30 or newer are found (`libibverbs-dev` or `rdma-core-devel`), the rdmacm engine
(`RDMACM_SUPPORT`) additionally needs librdmacm (`librdmacm-dev`); neither is available in
static executables. The libfabric engine is off by default: `make LIBFABRIC_SUPPORT=1` uses a
system-installed libfabric (development package, version 1.11 or newer) when `pkg-config` finds
one, and otherwise downloads and builds libfabric v2.7.0 into `external/` as a static library,
which needs autoconf, automake and libtool; `BUILD_LIBFABRIC=1` forces the own copy. The build
helpers and container images under `build_helpers/` use these defaults, so the static
executables contain the tcp engine only (a static build with the own libfabric, tcp provider
only, is possible with `LIBFABRIC_SUPPORT=1`). `elbencho --version` lists the engines as
features and shows the libfabric version in use.

The Ubuntu 26.04 and CUDA container images (`build_helpers/docker/Dockerfile.ubuntu2604.local`
and `Dockerfile.ubuntu-cuda-multiarch.local`) are the exception: they enable the ibverbs and
rdmacm engines explicitly and build their own libfabric with the verbs provider, so netbench
runs over InfiniBand and RoCE from these containers when the RDMA devices are passed through,
e.g.:

```bash
docker run --rm --net host --device /dev/infiniband --cap-add IPC_LOCK --ulimit memlock=-1:-1 \
    elbencho-local --service --foreground
```
