# elbencho

**A distributed storage benchmark for file systems and block devices with support for GPUs**

elbencho was inspired by traditional storage benchmark tools like [fio](https://github.com/axboe/fio), [mdtest](https://github.com/hpc/ior) and [ior](https://github.com/hpc/ior), but was written from scratch to replace them with a modern and easy to use unified tool for file systems and block devices.

## Features

* Unified latency, throughput, IOPS benchmark for file and block storage
* Supports local and shared storage through service mode
* For modern NVMe storage or classic spinning disk storage, on-prem and in the cloud
* CUDA-enabled transfer of data into GPU memory (Nvidia GPUDirect Storage support upcoming)
* Live statistics show how the system behaves under load
* Multi-threaded and async I/O support through libaio
* Results by first and by last finished thread
* CSV file output to easily create graphs in spreadsheet applications
* Data integrity verification option

## Usage

The built-in help (`elbencho --help`) provides simple examples to get started.

## Prerequisites

Building elbencho requires a C++14 compatible compiler, such as gcc version 5.x or higher.

### Install Dependencies for Debian/Ubuntu

```bash
sudo apt install build-essential debhelper devscripts git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libncurses-dev libnuma-dev lintian
```

### Install Dependencies for RHEL/CentOS 

```bash
sudo yum install boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel rpm-build
```

#### On RHEL / CentOS 7.x: Prepare Environment with newer gcc Version

Skip these steps on RHEL / CentOS 8.0 or newer.

```bash
sudo yum install centos-release-scl # for CentOS
# ...or alternatively for RHEL: yum-config-manager --enable rhel-server-rhscl-7-rpms
sudo yum install devtoolset-7
scl enable devtoolset-7 bash # alternatively: source /opt/rh/devtoolset-7/enable
```

The `scl enable` command enters a shell in which the environment variables are pointing to a newer gcc version. (The standard gcc version of the system remains unchanged.) Use this shell to run `make` later. The resulting executable can run outside of this shell.

## Build & Install

Start by cloning the main repository.

```bash
git clone https://github.com/breuner/elbencho.git
cd elbencho
```

`make help` will show you all build, install and rpm/deb packaging options.

<u>This is the standard build command</u> ("`-j8`" for 8 parallel build threads):

```bash
make -j8
```

<u>Alternatively, to enable GPU data transfers via CUDA</u> (requires Nvidia CUDA to be installed):

```bash
make -j8 CUDA_SUPPORT=1
```

You can run elbencho directly from the bin subdir (`bin/elbencho`), but you probably want to run `make rpm` or `make deb` now to build a package and install it. On Ubuntu, run this:

```bash
make deb
sudo apt install ./packaging/elbencho*.deb
```

**There you go. Happy benchmarking!**

## Now what?

Now comes the fun part. It's time to find out what your system can deliver, so that you know if you got what you expected and for how long this system will be able to support your plans for the future.

The built-in help (`elbencho --help`) provides many examples. You will be interested in access latency, IOPS and throughput.

If GPU data transfer performance is critical for you, e.g. because you are running DeepLearning applications, you will also want to include GPUs in your read/write benchmarks.

## Questions & Comments

In case of questions, comments, if something is missing to make elbencho more useful or if you would just like to share your thoughts, please feel free to contact sven.breuner[at]gmail.com
