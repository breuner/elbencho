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
  - [macOS Support](#macos-support)
  - [NVMe-oF Support](#nvme-of-support)
  - [Network Benchmarking](#network-benchmarking)
  - [Contributed Plugins](#contributed-plugins)

</details>

## Features

* Unified latency, throughput, IOPS benchmark for file, object & block storage
* Supports local and shared storage through distributed service mode
* For modern NVMe storage or classic spinning disk storage
* GPU storage access performance testing through Nvidia CUDA or GPUDirect Storage (GDS)
* Live statistics show how the system behaves under load
* Results by first and by last finished thread
* CSV file output to easily create graphs in spreadsheet apps or via elbencho-chart tool
* Data integrity verification option
* N-to-M network testing via TCP and RDMA

## Usage

The [built-in help](docs/usage/help.md) (`elbencho --help`) provides simple examples to get started.

  | ℹ️ **Note** |
  | :--- |
  | If you don't want to build elbencho from source, you can get elbencho pre-built for Linux & Windows from the [Releases section](https://github.com/breuner/elbencho/releases) and from [Docker Hub](https://hub.docker.com/r/breuner/elbencho). The [`elbencho-prof`](https://github.com/breuner/elbencho/tree/master/tools/elbencho-prof) tool is available to get started with automatic profile-driven tests for shared filesystems and S3 object stores. |

## Build Prerequisites

Building elbencho requires a C++17 compatible compiler, such as gcc version 7.x or higher.

### Dependencies for Debian/Ubuntu

```bash
sudo apt -y install build-essential cmake debhelper devscripts fakeroot git libaio-dev libboost-filesystem-dev libboost-json-dev libboost-program-options-dev libboost-thread-dev libcurl4-openssl-dev libnuma-dev lintian libssl-dev uuid-dev zlib1g-dev
```

### Dependencies for RHEL/CentOS/Rocky

```bash
sudo yum -y install boost-devel cmake gcc-c++ git libaio-devel libarchive libcurl-devel libuuid-devel make numactl-devel openssl-devel rpm-build zlib zlib-devel
```

#### On RHEL / CentOS 7.x: Prepare Environment with newer gcc Version

Skip these steps on RHEL/CentOS/Rocky 8.0 or newer.

```bash
sudo yum -y install centos-release-scl # for CentOS
# ...or alternatively for RHEL: yum-config-manager --enable rhel-server-rhscl-7-rpms
sudo yum -y install devtoolset-8
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

If your cluster is using Slurm to allocate nodes, you can find examples [here](docs/slurm-examples.md). Kubernetes/K8s examples are available [here](docs/k8s-examples.md).

If GPU data transfer performance is critical for you, e.g. because you are running DeepLearning applications, you will also want to include GPUs in your read/write benchmarks (`--gpuids`).

Instead of giving benchmark parameters on the command line, you can also use a config file (`--configfile myconfig.conf`). You can find an example [here](docs/example_configuration/).

### Results & Charts

Elbencho presents two [result columns](docs/result-columns-explanation.md): One showing the aggregate result from the beginning until the point in time when the fastest I/O thread finished its work ("First Done"). This result exludes the so-called tail phase where fewer threads are active and is sometimes referred to as stonewalling. The other one is the aggregate result from the beginning until the point in time when the slowest thread finally finished its fair share of the work ("Last Done").

If you are generating a result series, e.g. based on multiple iterations of the same test case or based on varying block sizes or thread counts, then consider using elbencho's json file option (`--jsonfile`) and the `elbencho-summarize-json` tool to get a summary of your results.

To generate charts from your result series, consider using elbencho's csv file option (`--csvfile`) and the `elbencho-chart` tool to easily generate a chart from your csv file. Alternatively, spreadsheet apps like Microsoft Excel or Google Sheets can be used for this.

See the [CSV documentation](docs/csv-docs.md) for detailed descriptions of fields.

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

##### Build elbencho with S3 Support

The static Linux executable in the [Releases section](https://github.com/breuner/elbencho/releases) includes S3 support, in case you prefer to use this instead of building your own version.

To build elbencho with S3 support, just add the `S3_SUPPORT=1` parameter to the make command.

The S3 support of elbencho is based on Amazon's official AWS SDK CPP. By default, the AWS SDK CPP will automatically get cloned from GitHub:

```bash
make S3_SUPPORT=1 -j $(nproc)
```

Alternatively, you can use your own copy of the AWS SDK CPP by adding the `AWS_INCLUDE_DIR` and `AWS_LIB_DIR` Makefile variables.

##### Build elbencho with S3-over-RDMA Support
S3-over-RDMA (s3rdma) support is available through contributed plugins. See [contrib/plugins/README.md](contrib/plugins/README.md).

#### macOS Support

Building elbencho on macOS requires homebrew. Run the following steps in a terminal.

Install Xcode command line tools:
```bash
xcode-select --install
```

Run the homebrew installation script:
```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Install the boost.org libraries through homebrew:
```bash
brew install boost cmake openssl
```

Clone the git repo and build elbencho with S3 support:
```bash
git clone https://github.com/breuner/elbencho ~/elbencho
cd ~/elbencho
make S3_SUPPORT=1 -j $(sysctl -n hw.ncpu)
```

That's it already.
```bash
bin/elbencho --help
```

#### NVMe-oF Support

NVMe-oF initiator support based on the SPDK toolkit can be enabled to test block devices directly, as an alternative to testing through the NVMe-oF stack of the Linux kernel. A guide on how to enable and use elbencho for NVMe-oF benchmarking is available in the `docs` subdir [here](docs/spdk.md).

#### Network Benchmarking

Elbencho has a built-in network benchmark mode. (See `elbencho --help-netbench` and [docs/netbench.md](docs/netbench.md).) In contrast to other common tools for network testing, this supports N-to-M communication, meaning multiple clients communicate in parallel with multiple servers, similar to typical shared storage access patterns.

The network benchmark mode supports mulitple engines:
* `tcp`: included by default.
* `ibverbs`: for InfiniBand/RoCE through libibverbs, built when the libibverbs development files are found on the system.
* `rdmacm`: for IndiniBand/RoCE connections established through the RDMA Connection Manager (RDMA CM), built when the rdmacm development files are found on the system.
* `libfabric`: for connections established through [libfabric](https://github.com/ofiwg/libfabric).
  * Libfabric is the chosen interface of the Ultra Ethernet Consortium and supports various providers, such as tcp, verbs, AWS EFA, HPE Slingshot.
  * Libfabric support can be enabled via `make LIBFABRIC_SUPPORT=1`, which uses a system-installed libfabric (development package, version 1.11 or newer) when `pkg-config` finds one, and otherwise downloads and builds its own copy into `external/` (needs autoconf, automake and libtool); `BUILD_LIBFABRIC=1` forces the own copy.

#### Contributed Plugins

Contributed extensions live as plugins outside of the core code in the `contrib/plugins` subdir. Each plugin gets compiled in via its own `make ELB_PLUGIN_<NAME>=1` option and activated at runtime via `--plugins <name>`. Currently available: `s3rdma_minio` (GPU-direct S3-over-RDMA via NVIDIA cuObject) and `s3rdma_cloudian` (S3-over-RDMA via Cloudian's aws-sdk-cpp fork). See [contrib/plugins/README.md](contrib/plugins/README.md) for details and for how to add a plugin.
