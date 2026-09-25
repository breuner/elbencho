# elbencho with S3 RDMA support

This adds support for S3 RDMA using Cloudian's RDMA-enabled aws-sdk-cpp fork.

Note that this is incompatible with S3 Transfer Manager.

## Requirements

1. Cloudian HyperStore 8.2.6.4
2. Cloudian [aws-sdk-cpp](https://github.com/cloudian/aws-sdk-cpp) 1.11.893+rdma
3. Nvidia cuObject client 1.0.0 (CUDA Toolkit 13.1.1)

## Build prerequisites

Build and install Cloudian aws-sdk-cpp as a static library:

```bash
git clone --depth 1 --recurse-submodules --shallow-submodules --branch 1.11.893+rdma https://github.com/cloudian/aws-sdk-cpp.git
cd aws-sdk-cpp
mkdir build
cd build
cmake .. -DBUILD_ONLY=s3 -DENABLE_TESTING=OFF -DBUILD_SHARED_LIBS=OFF
make -j$(nproc)
sudo make install
```

## Building

Build elbencho with S3 RDMA support using the installed AWS SDK:

```bash
make -j $(nproc) S3_SUPPORT=1 AWS_INCLUDE_DIR=/usr/local/include/ AWS_LIB_DIR=/usr/local/lib/
```

Support for S3 RDMA is automatically enabled when built with the Cloudian aws-sdk-cpp.

## Running

S3 RDMA is enabled with the `--s3rdma` option and the following requirements:

1. The Nvidia cuobjclient library can be loaded
2. The S3 endpoint supports RDMA operations

otherwise it falls back to TCP.

### GPU Direct Storage

GDS is supported by specifying the `--gds` and `--gpuids <ids>` options. The `--cufile` option uses the cuObj API for GDS when used with an S3 endpoint.  Use `nvidia-smi` to list the available GPU IDs on the system.

The benchmark runs in several modes depending on options:

* None: Data in host memory, transferred to/from objects using RDMA
* `--cuobjhostbufreg`: Data in host memory (pre-registered buffers), transferred to/from objects using RDMA
* `--gpuids <ids>`: Data in GPU memory, copied to/from host memory, then transferred to/from objects using RDMA
* `--gds --gpuids <ids>`: Data in GPU memory, transferred direct to/from objects using GPU Direct Storage RDMA
