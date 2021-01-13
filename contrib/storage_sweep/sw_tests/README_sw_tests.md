![Storage sweep logo](pics/storage_sweep.png)

Chin Fang <`fangchin[at]zettar.com`>, Palo Alto, California, U.S.A

<a name="page_top"></a>
Table of Contents
=================

   * [Layout and content](#layout-and-content)
      * [The real_tests's content](#the-real_testss-content)
      * [The unit_tests' content](#the-unit_tests-content)
   * [The environment](#the-environment)
   * [The included storage sweep results](#the-included-storage-sweep-results)
      * [The duration of each 3-run sweep session](#the-duration-of-each-3-run-sweep-session)

# Layout and content

This subdirectory consists of the two subdirectories:

1. `real_tests`
2. `unit_tests`

## The `real_tests`'s content

1. `losf`
2. `medium`
3. `large`
4. `overall` 

Each of the first three subdirectories contains respectively the sweep
results for

* **LOSF**   :1KiB <= file size < 1MiB
* **Medium** :1MiB <= file size < 1GiB
* **Large**  :1GiB <= file size <= 1024GiB (i.e. 1TiB)

The `overall` subdirectory contains results of a full sweep, going
from 1KiB all the way up to 1024GiB (i.e. 1TiB) with size increment in
powers of 2.

Each sweep is repeated 3 times (default).

[Back to top](#page_top)

## The `unit_tests`' content

It contains misc unit tests used while developing the two bash scripts
`graph_sweep.sh` and `mtelbencho.sh`.

[Back to top](#page_top)

# The environment

For an engineering documentation of this nature, foremost the test
environment should always be made clear to facilitate comparision and
reproduction, in additon to a precise description of the scope of the
test and how each one was run. 

The environment employed to produce included test results consists of
the following:

A single Docker container hosted on a [GIGABYTE
R281-NO0-00](https://www.gigabyte.com/us/Rack-Server/R281-NO0-rev-400#ov).
It is the only container hosted on the server while the tests were
conducted.

* 2x12-core Intel(R) Xeon(R) Gold 6146 CPU @ 3.20GHz
* 384GB RAM
* 8x NVMe 3.84TB SSDs in Redundant Arrays of Independent Disks
  (RAID)-0 (Intel
  [VROC](https://intel.ly/2LbI0ns))
  [[HGST Ultrastar SN200 Series NVMe SSD](https://bit.ly/2Msf23H)]
* [Mellanox ConnectX-5 dual-port 100G NICs](https://bit.ly/383r9fK)
* CentOS Linux release 7.9.2009 (Core)
* Filesystem: XFS

[Back to top](#page_top)

# The included storage sweep results

The following four files are actual sweep sessions:

1. [`real_tests/losf/graph_sweep_3_run_session.txt`](real_tests/losf/graph_sweep_3_run_session.txt)
2. [`real_tests/medium/graph_sweep_3_run_session.txt`](real_tests/medium/graph_sweep_3_run_session.txt)
3. [`real_tests/large/graph_sweep_3_run_session.txt`](real_tests/large/graph_sweep_3_run_session.txt)
4. [`real_tests/overall/graph_sweep_3_run_session.txt`](real_tests/overall/graph_sweep_3_run_session.txt)

Below are some context.

Zettar's focus is moving data at scale and speed, which always
involves mainly I/O driven workloads. Consequently, over the years, we
have devoted significant time and effort to storage benchmarking. Note
also that data mover software is at most 1/4th of the equation, as it
depends on the storage, computing, and networking (including network
security) stacks to work well.

Furthermore, the four stacks are inter-dependent. Tuning one stack may
affect the performance of others. So, the overall tuning is almost
always an interative process.  

Also, *for our purposes*, the storage tuning (aka storage sweep or
benchmarking) is aimed to provide the most I/O to each instance of the
data mover software. Thus, each sweep published herein is configured
for Zettar zx. Please see [Zettar zx Evaluation for ESnet
DTNs](https://www.es.net/assets/Uploads/zettar-zx-dtn-report.pdf).

For other data movers or applications, you are advised to adjust the
setup of your sweeps as appropriate.

[Back to top](#page_top)

## The duration of each 3-run sweep session

Please see the end of each included `graph_sweep_3_run_session.txt`,
also shown below for ease of reference.

1. **LOSF**   : `34m:19s`
2. **Medium** : `1h:1m:14s`
3. **Large**  : `1h:4m:20s`
4. **Overall**: `2h:40m:35s`

[Back to top](#page_top)
