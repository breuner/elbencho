## elbencho --help-dist

> **_NOTE:_**  This page has been auto-generated from built-in help text of the `elbencho` executable.

<pre><code>
Distributed benchmarking with multiple clients.

Usage:
  First, start elbencho in service mode on multiple hosts:
  $ elbencho --service [OPTIONS]

  Then run master anywhere on the network to start benchmarks on service hosts:
  $ elbencho --hosts HOST_1,...,HOST_N [OPTIONS] PATH [MORE_PATHS]

  When you're done, quit all services:
  $ elbencho --hosts HOST_1,...,HOST_N --quit

Basic Options:
  --hosts arg           List of hosts in service mode (separated by comma, 
                        space, or newline) for coordinated benchmark. When this
                        argument is used, this program instance runs in master 
                        mode to coordinate the given service mode hosts. The 
                        given number of threads, dirs and files is per-service 
                        then. (Format: hostname[:port])
  --hostsfile arg       Path to file containing line-separated service hosts to
                        use for benchmark. Lines starting with "#" will be 
                        ignored. (Format: hostname[:port])
  --service             Run as service for distributed mode, waiting for 
                        requests from master.
  --quit                Quit services on given service mode hosts.

Frequently Used Options:
  --zones arg           Comma-separated list of NUMA zones to bind this service
                        to. If multiple zones are given, then worker threads 
                        are bound round-robin to the zones. (Hint: See 'lscpu' 
                        for available NUMA zones.)
  --port arg            TCP communication port of service. (Default: 1611) 
                        Different ports can be used to run multiple service 
                        instances on different NUMA zones of a host. 0 lets the
                        OS choose a free port, which the service prints at 
                        start.

Miscellaneous Options:
  --nosvcshare          Benchmark paths are not shared between service hosts. 
                        Thus, each service host willwork on the full given 
                        dataset instead of its own fraction of the data set.
  --svcstats arg        Per-service statistics of a distributed run in the 
                        result table on the console and in the json result 
                        file. This value is a comma-separated list of the 
                        following: "elapsed" for the completion time of each 
                        service instance ordered by slowest thread; "lat" for 
                        the min/avg/max latency of each service instance; 
                        "percent" for the latency percentiles of each service 
                        instance (see "--latpercent9s"); "lathisto" for the 
                        latency histogram of each service instance (json result
                        file only); "minmax" for the service instances with the
                        lowest and the highest IOPS and throughput (rate and 
                        host name, plus the average over the services) based on
                        each service's own completion time, in netbench mode 
                        separately for servers and clients; "ping" for the 
                        response time of each service instance in the 
                        fullscreen live stats; "all" for all of these.
  --interrupt           Interrupt current benchmark phase on given service mode
                        hosts.
  --foreground          When running as service, stay in foreground and 
                        connected to console instead of detaching from console 
                        and daemonizing into background.

Examples:
  Start services on hosts node001 and node002:
    $ ssh node001 elbencho --service
    $ ssh node002 elbencho --service

  Run distributed test on node001 and node002, using 4 threads per service
  instance and creating 8 dirs per thread, each containing 16 1MiB files:
    $ elbencho --hosts "node00[1-2]" \
        -t 4 -d -n 8 -w -N 16 -s 1M /data/testdir

  Quit services on host IPs 192.168.0.1 and 192.168.0.3:
    $ elbencho --hosts "192.168.0.[1,3]" --quit
</code></pre>
