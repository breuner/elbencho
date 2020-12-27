## Layout and content

This subdirectory consists of four subdirectories:
1. `losf`
2. `medium`
3. `large`
4. `overall` 

The first three subdirectories each contains respectively a
`[lms]_nersc-tbn-6_tests_2020-12-27.txt`, which is the test results for

* **LOSF**   :1KiB <= file size < 1MiB
* **Medium** :1MiB <= file size < 1GiB
* **Large**  :1GiB <= file size <= 1024GiB (i.e. 1TiB)

The `overall` subdirectory also contains a
`o_nersc-tbn-6_tests_2020-12-27.txt`, which shows the results from a
full sweep, going from 1KiB all the way up to
1024GiB (i.e. 1TiB) with size increment in powers of 2.

Once the `mtelbencho.sh` is installed, assuming your `$PATH` variable
enables you to launch it on the CLI, please type 

`$ mtelbencho.sh -h` 

and review the examples 2-4 to see how the four files were generated.
New users can use such info to get started quickly.

## Duration of each sweep

Using the date command output at the top and bottom of each test result files,
it should be evident that given the environment described in the next section,
each single-run sweep took:
1. **LOSF**   : 10 minutes 27 seconds
2. **Medium** : 18 minutes 57 seconds
3. **Large**  : 21 minutes 2 seconds
4. **Overall**: 51 minutes 3 seconds

## The environment

In additon to a precise description of the scope of the test and
how each one was run, for an engineering documentation of this nature,
the test environment should always be made clear to facilitate
comparision and reproduction.  The environment employed to produce
these test results consists of the following:

A single Docker container hosted on a [GIGABYTE
R281-NO0-00](https://www.gigabyte.com/us/Rack-Server/R281-NO0-rev-400#ov).
It is the only container hosted on the server while the tests were
conducted.

* 2x12-core Intel(R) Xeon(R) Gold 6146 CPU @ 3.20GHz
* 384GB RAM
* 8x NVMe 3.84TB SSDs in Redundant Arrays of Independent Disks
  (RAID)-0 (Intel VROC) [HGST Ultrastar SN200 Series NVMe SSD]
* Mellanox ConnectX-5 dual-port 100G NICs
* CentOS Linux release 7.8.2003 (Core)
* Filesystem: XFS
