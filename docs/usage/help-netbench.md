## elbencho --help-netbench

> **_NOTE:_**  This page has been auto-generated from built-in help text of the `elbencho` executable.

<pre><code>
Network benchmarking between hosts.

Usage:
  Start a service on each host that takes part, servers and clients alike:
    $ elbencho --service [SERVICE OPTIONS]
  Run the benchmark from any host:
    $ elbencho --netbench --servers HOSTS --clients HOSTS [OPTIONS]

Basic Options:
  --netbench            Run network benchmarking between service instances 
                        instead of storage I/O.
  --servers arg         Comma-separated service hosts that act as servers. 
                        (Format: hostname[:port])
  --clients arg         Comma-separated service hosts that act as clients. Each
                        client thread runs one stream to one server, 
                        round-robin over the servers. (Format: hostname[:port])
  --sendsize arg        Bytes that a client thread sends to its server per 
                        round (same as "-b"). 0 for a receive-only stream. 
                        (Default: 1M)
  --recvsize arg        Bytes that the server sends back per round. A large 
                        send size with a small receive size simulates writes to
                        a storage server, the reverse simulates reads. 0 for a 
                        send-only stream. (Default: 1)
  -t [ --threads ] arg  Number of threads per service. Each client thread runs 
                        one stream, each server thread serves streams. 
                        (Default: 1)
  --tservers arg        Netbench threads per server, if fewer than "-t". 
                        (Default: "-t")
  --tclients arg        Netbench threads per client, if fewer than "-t". 
                        Without "-t", the thread count per service is the 
                        larger of "--tservers" and "--tclients". (Default: 
                        "-t")

Frequently Used Options:
  --timelimit arg       Stop the benchmark after this many seconds. (Default: 0
                        for no limit)
  --iodepth arg         Rounds in flight per stream. 1 means ping-pong, where 
                        the latency is the round trip time. (Default: 1)
  --netengine arg       Transport engine: "tcp" (plain TCP sockets), "ibverbs" 
                        (InfiniBand/RoCE via libibverbs), "rdmacm" (the same 
                        via RDMA CM, with ECE negotiation for Zero Touch RoCE) 
                        or "libfabric" (provider via "--netprov"). (Default: 
                        tcp)
  --netprov arg         libfabric provider, e.g. "tcp", "verbs;ofi_rxm" or 
                        "rdma" for the first RDMA provider; implies the 
                        libfabric engine. "list" lists the providers and 
                        interfaces of this host. (Default: tcp)
  --lat                 Show minimum, average and maximum round trip latency.

Service Options (given to the "--service" instances):
  --netdevs arg         Comma-separated network interfaces for the data 
                        endpoints, given as interface name, IP address, IP 
                        subnet ("172.200.0.0/16" for all interfaces in that 
                        subnet) or libfabric domain name. A server listens on 
                        each of them, a client binds its threads to them. 
                        Streams are placed between interfaces of the same 
                        subnet (see "--netcrosssubnet"), balanced over servers 
                        and interfaces; the interfaces are checked when a run 
                        starts. (Default: the interface of the service address 
                        for servers, one interface per subnet for clients)
  --netport arg         Fixed base port for the data endpoints of a server; 
                        endpoint number N listens on this port plus N. 
                        (Default: 0 for OS-chosen ports, announced via the 
                        service API)

Miscellaneous Options:
  -s [ --size ] arg     Bytes to send per client thread, or to receive for a 
                        receive-only stream. (Default: the largest value that 
                        fits the counters, so runs are ended by "--timelimit")
  --netcrosssubnet      Allow streams between interfaces in different IP 
                        subnets. By default, client and server interfaces are 
                        only paired within the same subnet.
  --netdevsubnets       Take the subnets for the stream placement from the IP 
                        subnets given in the services' "--netdevs" lists 
                        instead of from the interface netmasks (for interfaces 
                        with host addresses, e.g. "/32", in a routed network).
  --netmemmax arg       Limit for the send and receive buffer memory of a 
                        client or a server (see the Memory section of 
                        docs/netbench.md). Runs that would need more are 
                        rejected with the amounts they need. (Default: 1G)
  --netrecvpct arg      Receive buffers of a one-directional stream in percent 
                        of the sender's "--iodepth" (server side of send-only 
                        streams, client side of receive-only streams). Below 
                        100 provokes receiver-not-ready retries. (Default: 200)
  --netrnr arg          Receiver-not-ready retry timer of the ibverbs and 
                        rdmacm engines, as InfiniBand timer code 0-31 (1 = 10 
                        us, 12 = 0.64 ms, 0 = 655 ms). (Default: 1)
  --svcminmax           Show the clients and the servers with the lowest and 
                        highest IOPS and throughput (rate and host name, plus 
                        the average), to make outliers visible. Also adds these
                        values to the json result file.
  --serversfile arg     Path to file with line-separated server hosts.
  --clientsfile arg     Path to file with line-separated client hosts.

Examples:
  Simulate writes with 1MiB blocks, 8 streams per client, for 30 seconds:
    $ elbencho --netbench --servers s1,s2 --clients c[1-4] \
        --sendsize 1m --recvsize 1 -t 8 --timelimit 30

  Simulate 4KiB reads with 16 rounds in flight per stream:
    $ elbencho --netbench --servers s1 --clients c1 \
        --sendsize 1 --recvsize 4k --iodepth 16 --timelimit 30

  Send 10GiB per thread over RDMA in one direction only:
    $ elbencho --netbench --servers s1 --clients c1 --netengine ibverbs \
        --sendsize 4m --recvsize 0 -s 10g -t 4

  One server with 8 threads, 10 clients with 1 thread each, 4KiB ping-pong:
    $ elbencho --netbench --servers s1 --clients c[1-10] \
        --tservers 8 --tclients 1 --sendsize 4k --recvsize 4k --timelimit 30

  Services with a fixed interface and port range for the data endpoints:
    $ elbencho --service --netdevs eth1 --netport 20000

Details on the engines, interface selection, memory and stream placement are in
docs/netbench.md.
</code></pre>
