## Layout and content

This subdirectory consists of four subdirectories:
1. `losf`
2. `medium`
3. `large`
4. `overall` 

The first three subdirectories each contains a `tests_12242020.txt`,
which is the test results for

* **LOSF**   :1KiB <= file size < 1MiB
* **Medium** :1MiB <= file size < 1GiB
* **Large**  :1GiB <= file size <= 1024GiB (i.e. 1TiB)

The '`overall`' subdirectory also contains a `tests_12242020.txt`, which
shows the results from a full sweep, going from 1KiB all the way up to
1024GiB (i.e. 1TiB) with size increment in powers of 2.  The entire
sweep took *50 minutes 29 seconds*.

All result files also show how the wrapper was run for each test
run. New users can use such info to get started quickly.

## The environment

For an engineering documentation of this nature, the test environment
should always be made clear to facilitate comparision and
reproduction.  The environment employed to produce these test results
consists of the following:

A single Docker container hosted on a GIGABYTE R281-NO0-00.  It is the
only container hosted on the server while the tests were conducted.

* 2x12-core Intel(R) Xeon(R) Gold 6146 CPU @ 3.20GHz
* 384GB RAM
* 8x NVMe 3.84TB SSDs in Redundant Arrays of Independent Disks
  (RAID)-0 (Intel VROC) [HGST Ultrastar SN200 Series NVMe SSD]
* Mellanox ConnectX-5 dual-port 100G NICs
* CentOS Linux release 7.8.2003 (Core)
* Filesystem: XFS
