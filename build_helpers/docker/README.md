# elbencho

<img src="https://raw.githubusercontent.com/breuner/elbencho/master/graphics/elbencho-logo.svg" width="50%" height="50%" alt="elbencho logo" align="center"/>

**A distributed benchmark for file, object & block storage with support for GPUs**

For the full description, see elbencho's readme and changelog on github: https://github.com/breuner/elbencho

## Usage Examples

The default dockerfile defines the path to the elbencho executable as an entrypoint. This means you don't need to specify the name or path of the elbencho binary and just directly provide the elbencho parameters to the `docker run` command.

### Hello World

To just confirm that fetching of the container works, try showing elbencho's built-in help:

```bash
docker run -it breuner/elbencho --help
```

### Simple Benchmark

Use docker's `-v HOST_PATH:CONTAINER_PATH` option to make your test directory available inside the container. E.g. if your benchmark files should be in `/data` and you want to create a single 10GiB file:

```bash
docker run -v /data:/data -it breuner/elbencho /data/mytestfile -s 10G -w
```

### Distributed Mode

If you want to use elbencho's distributed mode for coordinated throughput tests from multiple nodes, the easiest way is to use Docker's `--net=host` option to make the elbencho service inside the container available under the IP address of the host on which it is running. Also, you will want to prevent the service from going into background (because otherwise Docker will think it can stop the container instance) and instead detach the container to run in the background (`--foreground` & `-d`).

Run this command on all nodes that should participate in your distributed storage benchmarks:

```bash
docker run -v /data:/data --net=host -dt breuner/elbencho --service --foreground
```

Now you're ready to control the service instances, again by using the `--net=host` option for the master instance. Here we write a single shared 10GiB file from all service instances:
```bash
docker run --net=host -it breuner/elbencho /data/mytestfile -s 10G -w --hosts HOST1,HOST2,...
```

When you're done with your distributed benchmarks and want to stop the service instances, use elbencho's `--quit` option to terminate them:

```bash
docker run --net=host -it breuner/elbencho --quit --hosts HOST1,HOST2,...
```

### GPUs & GPUDirect Storage (GDS)

To test GPU storage access performance through Nvidia CUDA or GPUDirect Storage (GDS), use the MagnumIO container.

Here is an example to read 128 large file via GDS, assuming a DGX-A100 style system with 8 GPUs and 8 NUMA zones:

```bash
nvidia-docker run --privileged -v /data:/data -it breuner/elbencho:master-magnum-io /data/mylargefile{1..128} -r -t 256 -b 4m --direct --zones "$(echo {0..7},)" --gpuids "$(echo {0..7})" --cufile --gdsbufreg 
```

If you don't have `nvidia-docker`, you can alternatively use `docker run --gpus all ...`.

## Docker Image Flavors

The default image (tagged "latest") is based on Alpine Linux for minimum size and only contains the elbencho main executable, no other tools. 

The Ubuntu and CentOS based images contain the full `.deb` / `.rpm` package installation. To use e.g. one of the contained elbencho tools, simply specify the tool name as an alternative entrypoint:

```bash
docker run -it --entrypoint elbencho-scan-path breuner/elbencho:master-ubuntu2004 --help
```

### S3 Support

Images with stable version tags include S3 support as of elbencho v2.0. Additionally, the following image flavor tags include S3 support:
- latest
- master-alpine
- master-centos7, master-centos8
- master-magnum-io
- master-sles15
- master-ubuntu2004

### Local Image Builds

To build docker images from your local elbencho git clone (e.g. because you modified the sources or want to build a particular previous version), you can find the corresponding dockerfiles in the `build_helpers/docker` subdir with a `.local` extension. To build a Ubuntu 20.04 image from your local sources, run this command:

```bash
docker build -t elbencho-local -f build_helpers/docker/Dockerfile.ubuntu2004.local .
```
