# elbencho

<img src="graphics/elbencho-logo.svg" width="50%" height="50%" alt="elbencho logo" align="center"/>

**A distributed storage benchmark for files, objects & blocks with support for GPUs**

elbencho was inspired by traditional storage benchmark tools like [fio](https://github.com/axboe/fio), [mdtest](https://github.com/hpc/ior) and [ior](https://github.com/hpc/ior), but was written from scratch to replace them with a modern and easy to use unified tool for file systems, object stores & block devices.

## Table of Contents
<details>
  <summary><b>(click to expand)</b></summary>

- [Features](#features)
- [Usage](#usage)
- [Build Prerequisites](#build-prerequisites)
  - [Dependencies for Debian/Ubuntu](#dependencies-for-debianubuntu)
  - [Dependencies for RHEL/CentOS](#dependencies-for-rhelcentos)
- [Build & Install](#build--install)
- [Now what?](#now-what)
  - [Results & Charts](#results--charts)
  - [Questions & Comments](#questions--comments)
- [Optional Build Features](#optional-build-features)
  - [Nvidia CUDA Support](#nvidia-cuda-support)
  - [Nvidia GPUDirect Storage \(GDS\) Support](#nvidia-gpudirect-storage-gds-support)
  - [S3 Object Storage Support](#s3-object-storage-support)

</details>

## Features

* Unified latency, throughput, IOPS benchmark for file, object & block storage
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

You can get elbencho pre-built for Linux & Windows from the [Releases section](https://github.com/breuner/elbencho/releases) and from [Docker Hub](https://hub.docker.com/r/breuner/elbencho). 

## Build Prerequisites

Building elbencho requires a C++17 compatible compiler, such as gcc version 7.x or higher.

### Dependencies for Debian/Ubuntu

```bash
sudo apt install build-essential debhelper devscripts fakeroot git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libncurses-dev libnuma-dev lintian libssl-dev
```

### Dependencies for RHEL/CentOS 

```bash
sudo yum install boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel openssl-devel rpm-build
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

`make help` will show you all build & install options.

(Note that S3 support is not enabled by default due to longer build time, but can easily be enabled. See the additional build info below.)

This is the standard build command:

```bash
make -j $(nproc)
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

If your cluster is using Slurm to allocate nodes, you can find examples [here](tools/slurm-examples.md).

If GPU data transfer performance is critical for you, e.g. because you are running DeepLearning applications, you will also want to include GPUs in your read/write benchmarks (`--gpuids`).

Instead of giving benchmark parameters on the command line, you can also use a config file (`--configfile myconfig.conf`). You can find an example [here](tools/example_configuration/).

### Results & Charts

Elbencho presents two result columns: One showing the aggregate status at the point in time when the fastest I/O thread finished its work ("First Done") and one for the aggregate end result at the point in time when the slowest thread finished its work ("Last Done"). Ideally, both are close together.

To generate charts from your result series, e.g. based on different block sizes or different thread counts, use elbencho's csv file option (`--csvfile`) and the `elbencho-chart` tool to easily generate a chart from your csv file. Alternatively, spreadsheet tools like Microsoft Excel or Google Sheets can be used for this.

### Questions & Comments

In case of questions, comments, if something is missing to make elbencho more useful or if you would just like to share your thoughts, feel free to contact me: sven.breuner[at]gmail.com

## Optional Build Features

`elbencho --version` shows which optional features are included in an executable.

#### Nvidia CUDA Support

CUDA support for GPU data transfers will automatically be enabled when CUDA development files (`cuda_runtime.h` and `libcudart.so`) are installed on the build system. Alternatively, elbencho CUDA support can be manually enabled or disabled. See `make help` for details.

#### Nvidia GPUDirect Storage (GDS) Support

GPUDirect Storage (GDS) support through the cuFile API will automatically be enabled when GDS development files (`cufile.h` and `libcufile.so`) are installed on the build system. Alternatively, elbencho GDS support can be manually enabled or disabled. See `make help` for details.

#### S3 Object Storage Support

Enabling S3 Object Storage support will automatically download a AWS SDK git repository of over 1GB size and increases build time from a few seconds to a few minutes. Thus, S3 support is not enabled by default, but it can easily be enabled as described below.

##### S3 Dependencies for RHEL/CentOS 8.0 or newer

```bash
sudo yum install cmake libarchive libcurl-devel libuuid-devel zlib zlib-devel
```

##### S3 Dependencies for Ubuntu 20.04 or newer

```bash
sudo apt install cmake libcurl4-openssl-dev uuid-dev zlib1g-dev
```

##### Build elbencho with S3 Support

The static Linux executable in the [Releases section](https://github.com/breuner/elbencho/releases) includes S3 support, in case you prefer to use this instead of building your own version.

To build elbencho with S3 support, just add the `S3_SUPPORT=1` parameter to the make command. (If you previously built elbencho without S3 support, then run `make clean-all` before this.)

The S3 support of elbencho is based on Amazon's AWS SDK CPP. Thus, you either need to provide it or elbencho needs to download and build it.

<ins>Option A</ins>: If you prefer to provide your own version of the AWS SDK CPP instead of having elbencho download it, then here is an example of a cmake command to generate an SDK that is compatible with elbencho and the corresponding elbencho build command:

```bash
# In the AWS SDK build dir:
cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local/ -DCMAKE_INSTALL_PREFIX=/usr/local/ -DBUILD_ONLY="s3;transfer" -DAUTORUN_UNIT_TESTS=OFF -DENABLE_TESTING=OFF -DBYO_CRYPTO=ON -DBUILD_SHARED_LIBS=OFF

make -j $(nproc)
sudo make install

# In the elbencho git clone top level dir:
make -j $(nproc) S3_SUPPORT=1 AWS_INCLUDE_DIR=/usr/local/include/ AWS_LIB_DIR=/usr/local/lib64/
```

<ins>Option B</ins>: If you are not sure whether you can provide a compatible build of the SDK or if you just generally prefer to have elbencho take care of the AWS SDK CPP build then use this command:

```bash
make S3_SUPPORT=1 -j $(nproc)
```
