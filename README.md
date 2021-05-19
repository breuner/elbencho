# elbencho

<img src="graphics/elbencho-logo.svg" width="50%" height="50%" alt="elbencho logo" align="center"/>

**A distributed storage benchmark for file systems and block devices with support for GPUs**

elbencho was inspired by traditional storage benchmark tools like [fio](https://github.com/axboe/fio), [mdtest](https://github.com/hpc/ior) and [ior](https://github.com/hpc/ior), but was written from scratch to replace them with a modern and easy to use unified tool for file systems and block devices.

## Features

* Unified latency, throughput, IOPS benchmark for file and block storage
* Supports local and shared storage through distributed service mode
* For modern NVMe storage or classic spinning disk storage
* GPU storage access performance testing through Nvidia CUDA or GPUDirect Storage (GDS)
* Live statistics show how the system behaves under load
* Multi-threaded and async I/O support through libaio
* Results by first and by last finished thread
* CSV file output to easily create graphs in spreadsheet apps or via elbencho-chart tool
* Data integrity verification option

## Usage

The built-in help (`elbencho --help`) provides simple examples to get started.

## Prerequisites

You can get elbencho pre-built from [Docker Hub](https://hub.docker.com/r/breuner/elbencho). Building elbencho requires a C++14 compatible compiler, such as gcc version 5.x or higher.

### Dependencies for Debian/Ubuntu

```bash
sudo apt install build-essential debhelper devscripts fakeroot git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libncurses-dev libnuma-dev lintian
```

### Dependencies for RHEL/CentOS 

```bash
sudo yum install boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel rpm-build
```

#### On RHEL / CentOS 7.x: Prepare Environment with newer gcc Version

Skip these steps on RHEL / CentOS 8.0 or newer.

```bash
sudo yum install centos-release-scl # for CentOS
# ...or alternatively for RHEL: yum-config-manager --enable rhel-server-rhscl-7-rpms
sudo yum install devtoolset-8
scl enable devtoolset-8 bash # alternatively: source /opt/rh/devtoolset-8/enable
```

The `scl enable` command enters a shell in which the environment variables are pointing to a newer gcc version. (The standard gcc version of the system remains unchanged.) Use this shell to run `make` later. The resulting executable can run outside of this shell.

## Build & Install

Start by cloning the main repository:

```bash
git clone https://github.com/breuner/elbencho.git
cd elbencho
```

`make help` will show you all build, install and rpm/deb packaging options.

This is the standard build command ("`-j8`" for 8 parallel build threads):

```bash
make -j8
```

You can run elbencho directly from the bin subdir (`bin/elbencho`), but you probably want to run `make rpm` or `make deb` now to build a package and install it. On Ubuntu, run this:

```bash
make deb
sudo apt install ./packaging/elbencho*.deb
```

**There you go. Happy benchmarking!**

## Now what?

Now comes the fun part: It's time to find out what your system can deliver.

The built-in help (`elbencho --help`) provides many usage examples. You will be interested in throughput and IOPS, typically for a single client and also for multiple clients. For the latter, see `--hosts` & `--service`.

If GPU data transfer performance is critical for you, e.g. because you are running DeepLearning applications, you will also want to include GPUs in your read/write benchmarks (`--gpuids`).

It is possible to configure elbencho from a configuration file in addition to the command line. For example running:

```bash
elbencho --configfile tools/example_configuration/randomWrite.elbencho /data
```

Is equivalent to:

```bash
elbencho -d -F -D --write -t 1 -n 1 -N 10 -s 1G --direct --iodepth 4 -b 1M --timelimit 10 /data
```

### Results & Charts

Elbencho presents two result columns: One showing the status when the first I/O thread finished its work and one for the end result when the last thread finished its work. Ideally, both are close together.

To generate charts from your result series, e.g. based on different block sizes or different thread counts, use elbencho's csv file option (`--csvfile`) and the `elbencho-chart` tool to easily generate a chart from your csv file. Alternatively, spreadsheet tools like Microsoft Excel or Google Sheets can be used for this.

### Questions & Comments

In case of questions, comments, if something is missing to make elbencho more useful or if you would just like to share your thoughts, feel free to contact me: sven.breuner[at]gmail.com

## Optional Build Features

`elbencho --version` shows which optional features are included in an executable.

#### Nvidia CUDA Support

CUDA support for GPU data transfers will automatically be enabled when CUDA development files (`cuda_runtime.h` and `libcudart.so`) are installed on the build system. Alternatively, elbencho CUDA support can be manually enabled or disabled. See `make help` for details.

#### Nvidia GPUDirect Storage (GDS) Support

GPUDirect Storage (GDS) support through the cuFile API will automatically be enabled when GDS development files (`cufile.h` and `libcufile.so`) are installed on the build system. Alternatively, elbencho GDS support can be manually enabled or disabled. See `make help` for details.

