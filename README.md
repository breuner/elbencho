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
  - [GPU-Direct S3-over-RDMA \(cuObject\) Support](#gpu-direct-s3-over-rdma-cuobject-support)
  - [macOS Support](#macos-support)

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

The [built-in help](docs/usage/help.md) (`elbencho --help`) provides simple examples to get started.

You can get elbencho pre-built for Linux & Windows from the [Releases section](https://github.com/breuner/elbencho/releases) and from [Docker Hub](https://hub.docker.com/r/breuner/elbencho).

## Build Prerequisites

Building elbencho requires a C++17 compatible compiler, such as gcc version 7.x or higher.

### Dependencies for Debian/Ubuntu

```bash
sudo apt -y install build-essential cmake debhelper devscripts fakeroot git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libcurl4-openssl-dev libnuma-dev lintian libssl-dev uuid-dev zlib1g-dev
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

To build elbencho with S3 support, just add the `S3_SUPPORT=1` parameter to the make command. (If you previously built elbencho without S3 support, then run `make clean-all` before this.)

The S3 support of elbencho is based on Amazon's AWS SDK CPP. Thus, you either need to provide it or elbencho needs to download and build it.

<ins>Option A</ins>: If you are not sure whether you can provide a compatible build of the SDK or if you just generally prefer to have elbencho take care of the AWS SDK CPP build then use this command:

```bash
make S3_SUPPORT=1 -j $(nproc)
```

<ins>Option B</ins>: If you prefer to provide your own version of the AWS SDK CPP instead of having elbencho download it, then here is an example of a cmake command to generate an SDK that is compatible with elbencho and the corresponding elbencho build command:

```bash
# In the AWS SDK build dir:
cmake ../aws-sdk-cpp -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local/ -DCMAKE_INSTALL_PREFIX=/usr/local/ -DBUILD_ONLY="s3;transfer" -DAUTORUN_UNIT_TESTS=OFF -DENABLE_TESTING=OFF -DBYO_CRYPTO=ON -DBUILD_SHARED_LIBS=OFF

make -j $(nproc)
sudo make install

# In the elbencho git clone top level dir:
make -j $(nproc) S3_SUPPORT=1 AWS_INCLUDE_DIR=/usr/local/include/ AWS_LIB_DIR=/usr/local/lib64/
```

#### GPU-Direct S3-over-RDMA (cuObject) Support

The `--cuobj` option performs single-part S3 GET/PUT using NVIDIA's cuObject (`cuObjClient`) API — the object-storage counterpart of `--cufile` (GDS). The object payload moves out-of-band over RDMA (directly to/from GPU memory when `--gpuids` is given, otherwise host/CPU memory), while a small body-less HTTP control request carries the `x-amz-rdma-*` protocol headers. It requires an RDMA-capable S3 endpoint that implements that protocol.

Support is auto-enabled when the cuObject development files (`cuobjclient.h` and `libcuobjclient.so`) are present and `S3_SUPPORT=1` is set; it additionally links `libibverbs`/`librdmacm`. See `make help` for the `CUOBJ_SUPPORT`, `CUOBJ_INCLUDE_PATH` and `CUOBJ_LIB_PATH` options.

**Version compatibility (important):** the `libcuobjclient` that elbencho links must be compatible with the cuObject version of your S3 server. The build picks `libcuobjclient` up from your CUDA install (or from `CUOBJ_LIB_PATH`/`CUOBJ_INCLUDE_PATH`), so use a CUDA / cuObject SDK whose `libcuobjclient` matches your server — do not mix major/minor versions. A client/server mismatch typically surfaces as RDMA buffer-registration failures or `retry exceeded` transfer errors even though the control-plane HTTP request succeeds.

**Runtime configuration:** cuObject reads a JSON config file (point `CUFILE_ENV_PATH_JSON` at it). The client NIC and RDMA properties must be set, for example:

```json
{
  "properties": {
    "allow_compat_mode": true,
    "use_pci_p2pdma": true,
    "rdma_peer_type": "dmabuf",
    "rdma_dev_addr_list": ["<client-nic-ipv4>"]
  }
}
```

**Constraints:**
* Single-part transfers only: the block size (`-b`) must equal the object size (`-s`), and `--iodepth=1` is required.
* An RDMA decline or failure is a hard error — there is no automatic HTTP fallback.
* The host needs RDMA-capable NICs (RoCEv2 or InfiniBand) and a sufficiently high locked-memory limit (`memlock`). For VRAM-direct transfers (`--gpuids`), GPUDirect RDMA must be working between the GPU and NIC (e.g. PCIe ACS redirect disabled on the data-path bridges).

Example (16 threads, 8 MiB objects, host/CPU buffers):

```bash
CUFILE_ENV_PATH_JSON=/path/to/cuobj.json \
  elbencho --s3endpoints https://S3SERVER:9000 --s3key KEY --s3secret SECRET \
    --cuobj -w -r -t 16 -s 8m -b 8m s3://mybucket
```

Add `--gpuids <id>` to the command above for VRAM-direct transfers.

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
