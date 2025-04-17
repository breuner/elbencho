## elbencho --help-dist

> **_NOTE:_**  This page has been auto-generated from built-in help text  of the `elbencho` executable.

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
  --hosts arg           Comma-separated list of hosts in service mode for 
                        coordinated benchmark. When this argument is used, this
                        program instance runs in master mode to coordinate the 
                        given service mode hosts. The given number of threads, 
                        dirs and files is per-service then. (Format: 
                        hostname[:port])
  --service             Run as service for distributed mode, waiting for 
                        requests from master.
  --quit                Quit services on given service mode hosts.

Frequently Used Options:
  --zones arg           Comma-separated list of NUMA zones to bind this service
                        to. If multiple zones are given, then worker threads 
                        are bound round-robin to the zones. (Hint: See 'lscpu' 
                        for available NUMA zones.)
  --port arg            TCP communication port of service. (Default: 1611) 
                        Different ports can be  used to run multiple service 
                        instances on different NUMA zones of a host.

Miscellaneous Options:
  --nosvcshare          Benchmark paths are not shared between service hosts. 
                        Thus, each service host willwork on the full given 
                        dataset instead of its own fraction of the data set.
  --svcelapsed          Show elapsed time to completion of each service 
                        instance ordered by slowest thread.
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
    $ elbencho --hosts node001,node002 \
        -t 4 -d -n 8 -w -N 16 -s 1M /data/testdir

  Quit services on host node001 and node002:
    $ elbencho --hosts node001,node002 --quit
</code></pre>
