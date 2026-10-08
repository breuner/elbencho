# Contributed Plugins

Plugins are contributed extensions of elbencho that live outside of the core code. Each plugin is
a self-contained directory here with its own sources, build rules and documentation. A plugin is
only compiled in when its make option is set, so it can never affect a build that does not select
it.

| Plugin | Description | Build option |
|---|---|---|
| [s3rdma_minio](s3rdma_minio/README.md) | GPU-direct S3-over-RDMA via NVIDIA cuObject | `ELB_PLUGIN_S3RDMA_MINIO=1` |
| [s3rdma_cloudian](s3rdma_cloudian/README.md) | S3-over-RDMA via Cloudian's aws-sdk-cpp fork | `ELB_PLUGIN_S3RDMA_CLOUDIAN=1` |

## Building & using a plugin

```bash
make clean-all # required when changing any optional build feature or plugin
make -j $(nproc) S3_SUPPORT=1 ELB_PLUGIN_<NAME>=1
```

`make help` lists the plugins and their options, `elbencho --version` shows the plugins compiled
into an executable and `elbencho --help-all` has a section for the options of each plugin.

At runtime, a plugin is inactive until it is selected via `--plugins <name>[,<name>...]`. This also
works in distributed mode: the coordinator activates the plugin on the service instances for the
run, so the same service instances can be used for runs with and without a plugin.

The S3-over-RDMA plugins are tested by `tests/run-tests.sh -r` against the test server in
[tools/s3rdma](../../tools/s3rdma/README.md), which also has a drop-in replacement for NVIDIA's
cuObject client library for hosts without the hardware that cuObject needs.

## Layout of a plugin

```
contrib/plugins/<name>/
    README.md      # what it does, how to build & use it, who contributed it
    plugin.mk      # build rules, only effective with ELB_PLUGIN_<NAME>=1
    source/        # the plugin's C++ sources
    patches/       # optional: patches for an external dependency of the plugin
```

`plugin.mk` is included by the main `Makefile` unconditionally, so that it can add its help text
via `PLUGIN_NAMES += <name>` and a `define PLUGIN_HELP_<name>` block. Everything else has to be
inside `ifeq ($(ELB_PLUGIN_<NAME>),1)`: dependency checks, `SOURCES +=`, `CXXFLAGS +=` (including
`-DELB_PLUGIN_<NAME>` and `-I` for its source dir), `LDFLAGS +=`, `PLUGINS_ENABLED += <name>` and
optionally `PLUGINS_EXTERNALS_ENV +=` for variables that `external/prepare-external.sh` should see
(e.g. `AWS_GIT_REPO`/`AWS_REQUIRED_TAG` for a different AWS SDK source, `AWS_PATCHES` for patch
files to apply to its fresh clone, `AWS_PREBUILT_LIBS` for a pre-built one).

## Plugin interface

A plugin implements the `Plugin` interface (`source/plugins/Plugin.h`) and registers its instance
with `ELB_REGISTER_PLUGIN(<Class>)` in its `.cpp` file. The interface covers everything outside
of the I/O path: command line options (parsed together with the core options, also from config
files), defaults, argument checks, and the transfer of the plugin's options to service instances.
Plugins define long option names only, and these must not collide with core options; elbencho
refuses to start otherwise.

The I/O path of a plugin is selected at compile time, so that it costs nothing when the plugin is
inactive and never adds virtual calls to the hot path. For S3, the seam is the `S3Transport` policy
class (`source/modes/s3/S3Transport.h`): a plugin provides its own transport class (typically
derived from `S3DefaultTransport`, checking its plugin's `isActive()` once per object) and points
`ELB_S3_TRANSPORT_HEADER` at its header in `plugin.mk`. Only one plugin can replace the S3
transport in a build. A transport that moves data directly from/to the GPU buffers reports this
via `isGpuDirect()`, so that the core skips its host/GPU copies like it does for cuFile.

## Conventions

* Source files carry the same SPDX header as the core files (GPL-3.0-only, "Sven Breuner and
  elbencho contributors").
* The plugin's `README.md` names the original contributor. It does not name a maintainer, so that
  contributing a plugin implies no obligation for its future maintenance. But it is usually
  highly desirable that a plugin contributor is willing to help with the plugin maintenance.
* Plugin options are not added to the bash completion file and plugin documentation stays in the
  plugin directory, so that core files and plugin files stay separate.
* Code should compile on Linux (amd64 & arm64), Cygwin and macOS, unless the plugin's dependencies
  are Linux-only anyway.
