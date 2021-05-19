# elbencho

<img src="https://raw.githubusercontent.com/breuner/elbencho/master/graphics/elbencho-logo.svg" width="50%" height="50%" alt="elbencho logo" align="center"/>

**A distributed storage benchmark for file systems and block devices with support for GPUs**

For the full description, see elbencho's readme and changelog on github: https://github.com/breuner/elbencho

## Usage

The default dockerfile defines the path to the elbencho executable as an entrypoint. This means you don't need to specify the name or path of the binary and just directly provide the parameters for elbencho:

```bash
docker run -it breuner/elbencho --help
```

To make your test directory available inside the container, use docker's -v option. E.g. if your benchmark files should be in /data and you want to create a single 10GiB file:

```bash
docker run -v /data:/data -it breuner/elbencho /data/mytestfile -s 10G -w
```

If you want to use elbencho's distributed mode for coordinated throughput tests from multiple clients, the easiest way is to use Docker's `--net=host` option to make the elbencho service inside the container available under the IP address of the host on which it is running. Also, you will want to prevent the service from going into background (because otherwise Docker will think it can stop the container instance) and instead detach the container to run in the background (`--foreground` & `-d`):

```bash
docker run -v /data:/data --net=host -dt breuner/elbencho --service --foreground
```

## Docker Image Flavors

The default image (tagged "latest") is based on Alpine Linux for minimum size and only contains the elbencho main executable, no other tools. 

The Ubuntu and CentOS based images contain the full `.deb` / `.rpm` package installation. To use e.g. one of the contained elbencho tools, simply specify the tool name as an alternative entrypoint:

```bash
docker run -it --entrypoint elbencho-scan-path breuner/elbencho:master-ubuntu2004 --help
```