# elbencho
A distributed benchmark for file systems and block devices

elbencho was inspired by other wide-spread storage benchmark tools like [fio](https://github.com/axboe/fio), [mdtest](https://github.com/hpc/ior) and [ior](https://github.com/hpc/ior) and combines their better parts into a single unified tool with some extra sugar on top. Test options include throughput, IOPS and access latency, ranging from low level block device performance up to lots of small file performance with multiple clients. Live statistics show how the system behaves under load and whether it is worth waiting for the end result.

## Usage

`bin/elbencho --help` provides the general overview, from which you can select the type of benchmarks you would like to run and see simple examples to get started.

## Building

Building requires a C++14 compatible compiler, such as gcc version 5.x or higher.

### Install Dependencies for Debian/Ubuntu

```sh
apt install git libaio-dev libboost-filesystem-dev libboost-program-options-dev libboost-thread-dev libncurses-dev libnuma-dev
```
 
### Install Dependencies for RHEL/CentOS 

```sh
yum install boost-devel git libaio-devel ncurses-devel numactl-devel
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

### Clone the Embedded HTTP Server Repository

The [embedded HTTP server](https://gitlab.com/eidheim/Simple-Web-Server) is used for communication in distributed benchmark mode.

```sh
external/prepare-external.sh
```

### Finally, the actual Build...
```sh
make -j8
```

There you go. Happy benchmarking!
