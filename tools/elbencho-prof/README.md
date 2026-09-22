# elbencho-prof

elbencho-prof is a profile-driven elbencho wrapper for **automatic performance tests from multiple clients**.

elbencho-prof supports **shared filesystems and S3 object storage**.

It can automatically generate test datasets of appropriate size (`--prep auto`) for the given number of hosts.

The actual bandwidth and IOPS test cases run time-limited to 60 seconds by default to avoid long wait times.

A result summary table will be shown on the console and results will also automatically get logged to the `results/` subdir.

## Automatic Dataset Sizing

elbencho-prof supports two different ways to generate a dataset for performance tests:

- _*Default:*_ elbencho-prof can automatically determine an appropriate dataset size for the given number of hosts by increasing the dataset size until the write phase runs for at least 60 seconds. This mode is the default for the first run if no `--prep` parameter is given. The automatic sizing phase can be triggered again later by using `--prep auto`.

  | ℹ️ **Note** |
  | :--- |
  | The automatic sizing phase (`--prep auto`) should be triggered again after significantly increasing the number of hosts, e.g. after first trying with a single host and then switching to multiple hosts. |

- A dataset size can be specified directly (e.g. `--prep 1t` to prepare a 1TiB sized dataset). elbencho-prof will check the existing dataset on startup and only triggers the preparation phase if the existing dataset does not meet the size specification.

## Download

Download the latest version to an existing shared filesystem mountpoint on a client:

```bash
cd /mnt/MySharedFS
git clone https://github.com/breuner/elbencho/ elbencho.git
cd elbencho.git/tools/elbencho-prof
```

## Quick Start

The following examples assume:
* Passwordless ssh is working between hosts. (No root privileges required)
* Shared mount exists on all given hosts and elbencho-prof is located on a shared mount.
* elbencho can be downloaded from GitHub if it is not already preinstalled on the hosts.

Use the built-in help (`-h`) to see options.


### For Shared Filesystems

```bash
# Create directory on shared filesystem for test dataset:
mkdir /mnt/MySharedFS/benchdir

# Automatically size a dataset and run basic performance tests from current host only...
./elbencho-prof.sh --dir /mnt/MySharedFS/benchdir

# Run basic performance tests from all given hosts and automatically re-size the dataset
# for the new number of hosts, e.g. after you have first tried with only a single client.
#
# Instead of hostnames or IPs, a path to a newline-separated hosts file can also be given.
./elbencho-prof.sh --dir /mnt/MySharedFS/benchdir --prep auto myclient001 "myclient[010-100]"

# To run or repeat only the read bandwidth test:
# (See "profiles/" subdir for available test profiles.)
./elbencho-prof.sh --dir /mnt/MySharedFS/benchdir --test file-read-bw myclient001 myclient005 "myclient[010-100]"

# Delete test dataset
rm -rf /mnt/MySharedFS/benchdir
```

### For S3 Object Storage

```bash
# Specify S3 endpoints and credentials via environment variables.
# Multiple endpoint IPs can be specified by using square bracket ranges, e.g. [1-10,12,14-20].
export AWS_ACCESS_KEY_ID="mys3user"
export AWS_SECRET_ACCESS_KEY="mys3secret"
export AWS_ENDPOINT_URL_S3="http://172.200.201.[1-10]"

# Automatically size a dataset and run basic performance tests from current host only...
# Use the "s3://" prefix to specify a bucket name, optionally followed by a prefix:
# (The bucket will get created if the user has bucket create permissions.)
./elbencho-prof.sh --dir s3://mybucket/benchtest/

# Run basic performance tests from all given hosts and automatically re-size the dataset
# for the new number of hosts, e.g. after you have first tried with only a single client.
#
# Instead of hostnames or IPs, a path to a newline-separated hosts file can also be given.
./elbencho-prof.sh --dir s3://mybucket/benchtest/ --prep auto myclient001 "myclient[010-100]"

# To run or repeat only the read bandwidth test:
# (See "profiles/" subdir for available test profiles.)
./elbencho-prof.sh --dir s3://mybucket/benchtest/ --test file-read-bw myclient001 myclient005 "myclient[010-100]"

# Delete test dataset.
# Note that aws cli does not understand square bracket ranges, so we pick a single IP address here.
aws s3 rm s3://mybucket/benchtest/ --recursive --endpoint-url http://172.200.201.1 --no-verify-ssl
```

## Result Output Example

In addition to the summary on the console, the full result details, elbencho command lines and summary will get logged to the `results/` subdir.

```
========================================================================================
 SUMMARY (2026-06-19 12:05:15 -0700) | Dataset: 4 TiB, 4096 files
========================================================================================
 Profile          Hosts  Threads  Block  Throughput      IOPS   Avg_IO_Lat       Elapsed
 ---------------  -----  -------  -----  ----------  --------  -----------  ------------
 prepare              8       64     8M   57.8 GB/s      6.9K            -       1m18.1s
 file-read-bw         8       64     1M  102.3 GB/s     97.6K            -         43.7s
 file-read-iops       8       64     4K    7.3 GB/s      1.8M            -        1m0.0s
========================================================================================
```

## Building A Result Tar File

The following command builds a tar archive of the results and stores it in the "`builds/`" subdir, e.g. for sharing with others.

(A downloaded elbencho binary will automatically be excluded from the archive to keep the size small.)

```
helpers/package.sh
```
