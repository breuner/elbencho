# elbencho
A distributed benchmark for file systems and block devices

elbencho was inspired by other wide-spread storage benchmark tools like [fio](https://github.com/axboe/fio), [mdtest](https://github.com/hpc/ior) and [ior](https://github.com/hpc/ior) and combines their better parts into a single unified tool with some extra sugar on top. Test options include throughput, IOPS and access latency, ranging from low level block device performance up to lots of small file performance with multiple clients. Live statistics show how the system behaves under load and whether it is worth waiting for the end result.

## Usage

`bin/elbencho --help` provides the general overview, from which you can select the type of benchmarks you would like to run and see simple examples to get started.

## Building

Building requires a C++14 compatible compiler, such as gcc version 5.x or higher.

### Install Dependencies for Debian/Ubuntu

```sh
apt install build-essential debhelper devscripts git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libncurses-dev libnuma-dev lintian
```

### Install Dependencies for RHEL/CentOS 

```sh
yum install boost-devel gcc-c++ git libaio-devel make ncurses-devel numactl-devel rpm-build
```

#### On RHEL / CentOS 7.x: Prepare Environment with newer gcc Version

Skip these steps on RHEL / CentOS 8.0 or newer.

```sh
yum install centos-release-scl
# ...or alternatively for RHEL: yum-config-manager --enable rhel-server-rhscl-7-rpms
yum install devtoolset-7
scl enable devtoolset-7 bash
```
The last command enters a shell in which the environment variables are pointing to a newer gcc version. (The standard gcc version of the system remains unchanged.) Use this shell to run ```make``` later. The resulting executable can run outside of this shell.

### Clone the Main Repository

```sh
git clone https://github.com/breuner/elbencho.git
cd elbencho
```

### Finally, the actual Build...

`make help` will show you all build, install and rpm/deb packaging options.

This is the standard build command:

```sh
make -j8  # "-j8" for 8 parallel build threads
```

To benchmark GPU data transfers via CUDA:

```sh
make -j8 CUDA_SUPPORT=1  # requires Nvidia CUDA to be installed
```

**There you go. Happy benchmarking!**

## Questions or Comments

Contact me at sven.breuner[at]gmail.com
