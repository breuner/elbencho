# Network Benchmarking

elbencho's netbench mode measures the network between hosts the way a storage benchmark uses
it: clients send blocks to servers and wait for the response, like write requests and their
acknowledgements or read requests and their data. Many clients talk to many servers at once,
over plain TCP, InfiniBand/RoCE or libfabric. All options are listed in the buit-in help
(`elbencho --help-netbench`, also available [here](usage/help-netbench.md)).

## Usage

### Starting the services

Every host that takes part runs an elbencho service, servers and clients alike:

```bash
elbencho --service
```

From a container image with RDMA support (see [Build](#build)), with the RDMA devices passed
through:

```bash
docker run --rm --net host --device /dev/infiniband --cap-add IPC_LOCK --ulimit memlock=-1:-1 \
    elbencho-local --service --foreground
```

Hosts with several network ports get `--netdevs` with the interfaces for the benchmark
traffic. A server opens data endpoints on each of them, a client sends from each of them:

```bash
elbencho --service --netdevs eth1,eth2                 # interface names
elbencho --service --netdevs 172.200.5.91,172.200.6.91 # IP addresses (IPv4 or IPv6)
elbencho --service --netdevs 172.200.0.0/16            # all interfaces with an address in that subnet
elbencho --service --netdevs mlx5_0,mlx5_1             # RDMA device names (ibverbs and libfabric engines)
```

Without `--netdevs`, a server uses the interface of the address that the coordinator names in
`--servers`, and a client one interface per IP subnet.

Connections are **subnet-aware**: a client port only talks to server ports in its own IP
subnet, so hosts whose ports are in separate networks need no further configuration, and the
streams are balanced over the servers and then over the ports of every server and every
client. `--netcrosssubnet` on the coordinator allows streams between subnets where they are
routed. The interfaces are resolved at the start of every run, so a service does not need a
restart when a port comes or goes.

### Running the benchmark

The benchmark is started from any host on the network (it does not need to be one of the services)
by naming the servers and the clients:

```bash
elbencho --netbench --netengine rdmacm --servers server[1-2] --clients client[1-4] -t 8 --timelimit 30
```

* Each client thread (`-t` per client host) runs one **stream** to one server; the server
  threads serve the streams. `--tservers`/`--tclients` give the two sides different thread
  counts (e.g. `--tservers 8 --tclients 1`).
* Per **round**, the client sends `--sendsize` bytes (the netbench name of `-b`, default 1 MiB)
  and the server answers with `--recvsize` bytes (default 1). A large send size with a small
  receive size simulates writes, the reverse simulates reads.
* `--iodepth` is the number of rounds in flight per stream. The default of 1 is a ping-pong,
  where the latency is the round trip time of one block and its response.
* The run ends after `--timelimit` seconds.
* `--netengine` selects the transport: `tcp` (default), `ibverbs` (InfiniBand/RoCE), `rdmacm`
  (the same with RDMA CM connection setup) or `libfabric` (`--netprov` names the provider).

| Scenario | Options |
| :--- | :--- |
| Writes with 1 MiB blocks (the default) | `--sendsize 1m --recvsize 1` |
| 4 KiB reads, 16 requests in flight per stream | `--sendsize 1 --recvsize 4k --iodepth 16` |
| Latency and the slowest/fastest service | `--lat --svcstats minmax` |

The result reports the phase as `NET/<engine>` from the clients' view: the "send" rows count
the blocks and bytes the clients sent, the "recv" rows the responses they received (in the json
and csv files these are the `rwmix_read` values), the latency is the client's round trip time.
The servers see the same traffic from the other side and are not added to these totals.
`--svcstats minmax` adds the slowest and the fastest client and server (a broken cable or a
misconfigured port stands out there), and `--log 1` on the coordinator shows the interfaces
every service reported.

## Implementation Details

### Connections

A run is set up in these steps:

1. The coordinator asks every service for its current interfaces (the resolved `--netdevs`
   entries). The reply also carries a per-process service ID and the hostname: entries that
   reach a service already listed under another address (e.g. a pool of floating IPs) are
   aliases and are left out with a `NOTE` (`--log 1` names them; a service listed as server and
   as client loses its client entry), and services given as IP address are shown as
   `address:port (hostname)` in messages. Literal duplicates (the same `host:port` twice) are
   rejected.
2. The coordinator places every client stream on a server interface and sends every service
   its part of the placement. Two interfaces are in the same subnet when each address lies in
   the other's network (differing netmasks do not matter); IPv4 and IPv6 are never paired. A
   subnet in `--netdevs` narrows the netmask when it is more specific (`172.200.5.0/24` keeps
   two networks apart although both ports carry a /16), and `--netdevsubnets` on the
   coordinator replaces the netmasks by the `--netdevs` subnets altogether, for ports with
   host addresses (`/32`) in routed networks. The streams are balanced over the servers first,
   then over the interfaces of each server, then over the interfaces of each client, and the
   streams of a server go round-robin over its threads. A client that shares no subnet with any
   server, or a server no client can reach, is an error before anything is prepared; groups
   with uneven stream counts (e.g. a client with a broken cable) get a `NOTE`.
3. Each server thread opens one data endpoint per interface. Each client thread registers its
   stream at its server through the service HTTP API, gets the endpoint details back and
   verifies the data path with a small ping message. This is the same for all engines.
4. The streams run independently of each other, so a slow port does not throttle the other
   streams of its host; it shows up as the gap between the "First Done" and "Last Done"
   columns, and each stream logs its own throughput when it finishes (`--log 2` on the
   services).

The control connections (coordinator to services, clients to their servers) use the service
addresses and are not subject to the placement rules.

**Example:** four clients and two servers, every host with one port in network A
(`10.1.0.0/24`) and one in network B (`10.2.0.0/24`), all services started with
`--netdevs 10.1.0.0/24,10.2.0.0/24`:

```bash
elbencho --netbench --servers 10.1.0.1,10.1.0.2 --clients 10.1.0.[3-6] -t 2
```

A client spreads its threads evenly over its ports, so with `-t 2` each client port has one
stream. The stream of a client port can only go to a server port in the same network, and the
servers are filled evenly, so the clients alternate between the two servers and every server
port ends up with two streams, from two different clients:

```
 network A (10.1.0.0/24)                        network B (10.2.0.0/24)

 client1 10.1.0.3 -------> 10.1.0.1 server1     client1 10.2.0.3 -------> 10.2.0.2 server2
 client2 10.1.0.4 -------> 10.1.0.2 server2     client2 10.2.0.4 -------> 10.2.0.1 server1
 client3 10.1.0.5 -------> 10.1.0.1 server1     client3 10.2.0.5 -------> 10.2.0.2 server2
 client4 10.1.0.6 -------> 10.1.0.2 server2     client4 10.2.0.6 -------> 10.2.0.1 server1
```

With `-t 4` each client port has two streams, and they go to the two server ports of its
network, so every server port receives one stream from every client (four in total); the same
for all four clients:

```
 client1 10.1.0.3 ---+---> 10.1.0.1 server1     client1 10.2.0.3 ---+---> 10.2.0.1 server1
                     +---> 10.1.0.2 server2                         +---> 10.2.0.2 server2
```

With `-t 1` only network A would be used (four streams, two per server), because the servers
are filled before the ports.

With `--log 2` the coordinator prints the placement of streams.

### Interfaces and ports

* A `--netdevs` entry is an interface name (resolved to the address of the family of the
  service address), an IP address, an IP subnet (every interface with an address in it) or an
  RDMA device or libfabric domain name (`mlx5_0`, `cxi0`, `rdmap0s6-rdm`; not for `rdmacm`).
* A service refuses to start when none of its entries matches an interface and warns (on the
  console, before it goes into the background) about entries that match nothing while others
  do. The entries are resolved again at the start of every run.
* A client without `--netdevs` uses the first interface of each subnet; the kernel routes
  through the first interface of a subnet anyway, so list the interfaces explicitly to use
  several in one subnet.
* The data endpoints use ports chosen by the OS, announced through the service API.
  `--netport <port>` on a server service fixes a base port instead; endpoint number N listens
  on this port plus N.

> **_NOTE:_** **Multiple host interfaces in one subnet** require source-based policy routing
(`ip rule add from <own address> lookup <table>` plus a route for the subnet through the matching
device in that table) and `arp_ignore=1`/`arp_announce=2` to work properly.
> (`ip route get <peer> from <own address>` can be used to validate routes.)

### Engines

All services of a run use the engine given to the coordinator, and a service can run a
different engine in every run without being restarted. Streams of all engines wait for
completions through file descriptors (`poll()`), there is no busy polling.

| Engine | Transport | Needs | File descriptors on a server |
| :--- | :--- | :--- | :--- |
| `tcp` (default) | one TCP socket per stream | nothing | 1 per stream (plus 1 listener per endpoint until the phase starts) |
| `ibverbs` | one InfiniBand/RoCE reliable connection (RC queue pair) per stream | libibverbs (rdma-core 30 or newer) | 1 per endpoint (thread x interface), plus 2 per RDMA device |
| `rdmacm` | the same connections, set up by RDMA CM (ECE negotiation) | libibverbs and librdmacm (rdma-core 30 or newer) | 2 per endpoint, plus librdmacm's device contexts |
| `libfabric` | libfabric reliable datagram endpoint, provider via `--netprov` | libfabric 1.11 or newer | about 10 per endpoint, plus 1 per stream for the tcp provider |

* **tcp:** non-blocking sockets with `TCP_NODELAY`, IPv4 and IPv6. Both sides know the message
  sizes, so the byte stream needs no framing. A server listens on one socket per endpoint and
  closes it when the phase starts.
* **ibverbs:** InfiniBand and RoCE (v2 preferred over v1) straight on libibverbs. Queue pair
  numbers, GIDs and sequence numbers are exchanged through the service API, so rdma_cm and its
  ECE negotiation are not involved. An IP address selects the RoCE GID that carries it, or the
  InfiniBand port behind an IPoIB interface. Blocks go via RDMA send/receive; a server posts a
  receive for every block its clients can have in flight (see below), so receiver-not-ready
  retries are rare (`--netrnr` sets their timer). `--netport` has no meaning here.
* **rdmacm:** the same connections and data path, but set up by RDMA CM through librdmacm: a
  server listens on its interface address (`--netport` fixes the port) and the client resolves
  and connects to it like any rdma_cm application. The adapters negotiate the connection
  parameters among themselves, including ECE (Enhanced Connection Establishment), which enables
  vendor features such as the congestion control mode of Zero Touch RoCE. So this is the engine
  to compare against `ibverbs` when such features are in question: identical traffic, with and
  without ECE. `--log 1` on the clients logs the negotiated ECE vendor and options per
  connection (or "no ECE").
* **libfabric:** `--netprov <name>` names the provider (default `tcp`) and implies the engine;
  `--netprov rdma` takes the first available RDMA provider (verbs, efa, cxi, ...) and
  `elbencho --netbench --netprov list` lists the providers and interfaces of a host. All hosts
  of a run need the same provider, and ideally the same libfabric version: the tcp provider was
  rewritten in libfabric 1.18 and does not interoperate with older versions (verbs worked
  across 1.17 and 2.7 in our tests); a server logs a note when a client connects with a
  different version. libfabric is tuned and debugged through its `FI_*` environment variables
  (`FI_LOG_LEVEL=warn`). The verbs provider moves blocks of 128 KiB and more via RDMA reads.
  A recent libfabric (2.7 or newer, which `make BUILD_LIBFABRIC=1` builds) is recommended:
  older versions were seen crashing while closing endpoints after interrupted runs with many
  streams, which elbencho works around by draining its endpoints before closing them.

### Memory and limits

A server posts a receive buffer for every block its clients can have in flight, spread over
its threads and interfaces, and a client one buffer per round in flight for the responses, so
no message has to wait inside the transport.

* **Ping-pong streams** pace themselves (a side only sends again after the other side reposted
  its buffer and answered), so the RDMA engines never see a receiver-not-ready condition. With
  libfabric, large blocks are pulled via RDMA read as soon as they are announced, and the
  `verbs;ofi_rxm` stack has credit-based flow control on top.
* **One-directional streams** (`--recvsize 0` or `--sendsize 0`) have no such pacing: the
  sender continues as soon as the receiving adapter acknowledged a block, while the receiver
  reposts the buffer only after its thread saw the completion. The receiving side therefore
  posts `--netrecvpct` percent of the sender's `--iodepth` as buffers (default 200, rounded up,
  at least one); below 100 provokes receiver-not-ready (RNR) retries on purpose. The ibverbs
  and rdmacm engines retry after the timer `--netrnr <code>`, an InfiniBand RNR NAK timer code
  from 0 to 31 (1 = 10 us, the default; 12 = 0.64 ms; 0 = 655 ms, the kernel default of
  rdma_cm); the libfabric verbs provider takes `FI_VERBS_MIN_RNR_TIMER` instead, so the option
  is rejected for other engines. The adapter counters show the retries (`rdma statistic` or
  `/sys/class/infiniband/<dev>/ports/1/hw_counters/rnr_nak_retry_err`). With the tcp engine,
  fewer buffers than blocks in flight just leave data in the socket buffer for a while.
* **Buffer memory:** a client thread needs `--iodepth` x `--sendsize` plus its receive buffers
  x `--recvsize`, a server (streams per server) x (receive buffers per stream) x `--sendsize`
  plus one `--recvsize` response buffer per endpoint, where the receive buffers per stream are
  `--iodepth` for ping-pong streams and `--netrecvpct` of it for one-directional ones.
  `--netmemmax <size>` (default 1 GiB, 0 for no limit) rejects a run whose buffers would exceed
  that amount on a client or a server; the error message states both amounts with their
  calculation.
* **Locked memory:** the RDMA engines pin the buffers above and little else, but the locked
  memory limit (`ulimit -l`) has to cover them; RDMA users typically run with `unlimited`. The
  libfabric verbs provider additionally pins about 65 MiB per data endpoint at libfabric's
  defaults (client threads included), so a limit of a few MiB, as systemd gives its services
  by default, fails the endpoint setup with "Cannot allocate memory". Most of that is sized by
  `FI_UNIVERSE_SIZE` (expected peers per endpoint, default 256 in libfabric 1.x, 1024 in 2.x,
  read once per process): lower it for hosts with many threads or a limited memlock, and keep
  it above the number of client threads per server endpoint. Each client stream also costs the
  server about 2 MiB of libfabric-internal receive buffers with the verbs provider
  (`FI_OFI_RXM_MSG_RX_SIZE` x `FI_OFI_RXM_BUFFER_SIZE`).
* **File descriptors:** elbencho raises its soft open files limit to the hard limit. With
  libfabric, the hard limit (`ulimit -Hn`) of a server has to cover threads x interfaces x 10
  plus the limit's own baseline; the other engines need one or two descriptors per stream or
  endpoint (see the engines table).

### Build

Netbench mode is always built; the tcp engine needs no library. `elbencho --version` lists the
engines as features and shows the libfabric version in use.

| Engine | Build |
| :--- | :--- |
| `ibverbs` | `IBVERBS_SUPPORT`, auto-detected: libibverbs development files of rdma-core 30 or newer (`libibverbs-dev`, `rdma-core-devel`); not in static executables |
| `rdmacm` | `RDMACM_SUPPORT`, auto-detected: additionally librdmacm (`librdmacm-dev`); not in static executables |
| `libfabric` | off by default; `make LIBFABRIC_SUPPORT=1` uses a system libfabric (development package, 1.11 or newer) when `pkg-config` finds one, else downloads and builds v2.7.0 into `external/` as a static library (needs autoconf, automake, libtool); `BUILD_LIBFABRIC=1` forces the own copy |

The build helpers and container images under `build_helpers/` use these defaults, so the static
executables contain the tcp engine only. The Ubuntu 26.04 and CUDA images
(`build_helpers/docker/Dockerfile.ubuntu2604.local` and `Dockerfile.ubuntu-cuda-multiarch.local`)
are the exception: they enable the ibverbs and rdmacm engines explicitly and build their own
libfabric with the verbs provider, so netbench runs over InfiniBand and RoCE from these
containers when the RDMA devices are passed through (see the `docker run` example above).
