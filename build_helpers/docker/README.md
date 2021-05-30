# elbencho

<img src="https://raw.githubusercontent.com/breuner/elbencho/master/graphics/elbencho-logo.svg" width="50%" height="50%" alt="elbencho logo" align="center"/>

**A distributed storage benchmark for file systems and block devices with support for GPUs**

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


## Docker Image Flavors

The default image (tagged "latest") is based on Alpine Linux for minimum size and only contains the elbencho main executable, no other tools. 

The Ubuntu and CentOS based images contain the full `.deb` / `.rpm` package installation. To use e.g. one of the contained elbencho tools, simply specify the tool name as an alternative entrypoint:

```bash
docker run -it --entrypoint elbencho-scan-path breuner/elbencho:master-ubuntu2004 --help
```

