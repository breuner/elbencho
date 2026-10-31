# elbencho CSV and JSON Result Files

This document describes the fields of the csv result file (`--csvfile`), of the json result file (`--jsonfile`) and of the live statistics csv file (`--livecsv`).

# CSV Result File

elbencho supports operations against both directories (for example `--mkdirs`) and files (for example `--write`). Each elbencho operation however only deals with one of these so the generic name `entry` is used in the CSV fields. See the `operation` column for the row to determine if the operation was working with directory or file `entries`.

As mentioned in the [main README file](README.md), elbencho presents two result columns: One showing the aggregate status at the point in time when the fastest I/O thread finished its work ("First Done") and one for the aggregate end result at the point in time when the slowest thread finished its work ("Last Done"). Results for each of these are stored separately in the CSV with columns appended with `[first]` and `[last]` respectively.

## Fields
* `ISO date` - timestamp when the row was written to the CSV file when the test completed
* `label` - the label, if any, specified for this run on the command line (see `--label`)
* `path type` - type of the given paths (dir, file, blockdev, bucket, object, hdfs or net)
* `paths` - number of paths passed into elbencho
* `hosts` - number of hosts generating load
* `threads` - number of threads _per host_ generating load
* `dirs` - number of directories _per thread_
* `files` - number of files _per thread_
* `file size` - size of the files in bytes
* `block size` - block size in bytes
* `direct IO` - 1 if direct IO is used, else 0
* `random` - 1 if random operations were used, else 0 (the default)
* `random aligned` - 1 if random operations were block size aligned (the default), 0 if they were not (see `--norandalign`) or blank if no random operations were used
* `IO depth` - IO depth used
* `shared paths` - 1 if the paths were shared by all hosts (the default), else 0 (see `--nosvcshare`)
* `truncate` - 1 if files were truncated before writing (see `--trunc`), else 0 (the default)
* `operation` - file operation being performed (READ, WRITE, etc)
* `time ms [first]` & `[last]` - time in milliseconds since the test started and the first and last thread finished
* `CPU% [first]` & `[last]` - CPU percentage of the run as seen by the first and last thread finished
* Operation results
  * `entries/s [first]` & `[last]` - number of entries per second for the first and last thread finished
  * `IOPS [first]` & `[last]` - number of IOPS for the first and last thread finished
  * `MiB/s [first]` & `[last]` - throughput in MiB/s for the first and last thread finished
  * `entries [first]` & `[last]` - total entries created by the first and last thread finished
  * `MiB [first]` & `[last]` - total MiB read/written by the first and last thread finished
  * `Ent lat us [min]` & `[avg]` & `[max]` - time in microseconds to complete an entry (file or directory)
  * `IO lat us [min]` & `[avg]` & `[max]` - time in microseconds to complete an IOP, that is: IO latency
* When using `--rwmixpct` to do mixed reads and writes concurrently, the `entries/s` and related fields above report the data for the WRITE operations. The results of the READ operations are stored in the `rwmix read` fields.
  * `rwmix read entries/s [first]` & `[last]`
  * `rwmix read IOPS [first]` & `[last]`
  * `rwmix read MiB/s [first]` & `[last]`
  * `rwmix read entries [first]` & `[last]`
  * `rwmix read MiB [first]` & `[last]`
  * `rwmix read Ent lat us [min]` & `[avg]` & `[max]`
  * `rwmix read IO lat us [min]` & `[avg]` & `[max]`
* `version` - version of elbencho
* `command` - elbencho command line used

# JSON Result File

The json result file (`--jsonfile`) is in json-lines format: every benchmark phase appends one json object as a single line, so a file can collect the results of several phases and runs. Every scalar is written as a quoted string, also numbers and booleans. Counters that are zero in a phase are left out (e.g. there is no `bytes` in a `MKDIRS` phase), and optional sections only appear when their option is given. The `elbencho-summarize-json` tool summarizes the phases of a file.

## Top-level fields
* `phase_type` - the benchmark phase (READ, WRITE, RWMIX50, MKDIRS etc)
* `phase_id` - unique id of this run of the phase
* `label` - the label, if any, specified for this run on the command line (see `--label`)
* `iso_start_date` - timestamp of the start of the phase
* `config` - the configuration of the run, see below
* `first_done` - the aggregate result from the beginning of the phase until the first/fastest thread finished its fair share of the work, see below
* `last_done` - the aggregate result from the beginning of the phase until the last/slowest thread finished its fair share of the work, see below

## `config` fields
* `path_type` - type of the given paths (dir, file, blockdev, bucket, object, hdfs or net)
* `paths` - number of paths passed into elbencho
* `hosts` - number of hosts generating load (1 in standalone mode)
* `threads` - number of threads _per host_ generating load
* `dirs` & `files` - number of directories and files _per thread_ (directory mode)
* `custom_tree_dirs`, `custom_tree_files_shared` & `files_not_shared` - number of directories, shared files and non-shared files of a custom tree (directory mode with `--treefile`, instead of `dirs` and `files`)
* `file_size` - size of the files in bytes
* `block_size` - block size in bytes
* `direct_io` - true if direct IO is used, else false
* `random_offsets` - true if random offsets were used, else false
* `random_aligned` - true if the random offsets were block size aligned, false if not (see `--norandalign`); only with random offsets
* `io_depth` - IO depth used
* `shared_service_paths` - true if the paths were shared by all hosts (the default), false if not (see `--nosvcshare`); only in distributed mode
* `truncate_files` - true if files were truncated before writing (see `--trunc`), else false; not for block devices and S3
* `version` - version of elbencho
* `command` - elbencho command line used (without the S3 secret key)

## `first_done` and `last_done` fields
* `elapsed_time_ms` - time in milliseconds from the start of the phase until the first or last thread finished
* `entries/s` - number of entries (files or directories) per second
* `iops` - number of IO operations per second
* `bytes/s` - throughput in bytes per second
* `entries` - total number of entries done
* `bytes` - total number of bytes read/written
* `cpu%` - CPU utilization in percent (in distributed mode the average over the service instances)
* `rwmix_read` - when using `--rwmixpct` or `--rwmixthr` to do mixed reads and writes concurrently (or in netbench mode), the fields above report the write (send) operations and this subtree repeats `entries/s`, `iops`, `bytes/s`, `entries` and `bytes` for the read (receive) operations
* `latency` - only in `last_done`, with a subtree `entries` for the time to complete an entry (file or directory) and a subtree `IO` for the time to complete an IO operation, each with:
  * `min_us`, `avg_us` & `max_us` - minimum, average and maximum latency in microseconds (see `--lat`)
  * `percentiles_us` - latency percentiles in microseconds (see `--latpercent` and `--latpercent9s`), keyed by percentage, e.g. `"1"`, `"50"`, `"75"`, `"99"`, `"99.9"`. A value is the upper bound of the histogram slot that the percentile falls into; for the open-ended top slot it is only a lower bound
  * `histogram` - the latency histogram as a `buckets` array of `{"range": "20-21us", "count": 2}` elements, only with the ranges that have values (see `--lathisto` and `--lathistogrpd`); the top range is open-ended ("...+us")
  * `rwmix_read` - the same latency fields for the read (receive) operations in a read/write mix or in netbench mode (for `histogram`, the `rwmix_read` subtree is inside `histogram`)
* `services` - only in `last_done` of a distributed run with `--svcstats` (named `clients` in netbench mode, with an additional `servers` subtree for the server services):
  * `iops` & `bytes/s` - with `--svcstats minmax`: `min`, `min_host`, `avg`, `max` and `max_host` over the service instances, each service's rate being its own total over the completion time of its own slowest thread
  * `instances` - with `--svcstats elapsed`, `lat`, `percent` or `lathisto`: one element per service instance, ordered by completion time (fastest first), with `host`, `elapsed_time_ms` (with `elapsed`) and `latency` with the same `entries`/`IO` subtrees as above, containing `min_us`/`avg_us`/`max_us` (with `lat`), `percentiles_us` (with `percent`) and `histogram` (with `lathisto`), plus a `rwmix_read` child inside `entries`/`IO` for the read operations

Example (reformatted to multiple lines; a real file has one phase per line):
```
{"phase_type": "WRITE", "phase_id": "78cf6be7-...", "label": "mylabel", "iso_start_date": "2026-10-31T23:05:27.865+0100",
 "config": {"path_type": "dir", "paths": "1", "hosts": "1", "threads": "2", "dirs": "2", "files": "50", "file_size": "4096",
            "block_size": "4096", "direct_io": "false", "random_offsets": "false", "io_depth": "1", "truncate_files": "false",
            "version": "3.3-4", "command": "..."},
 "first_done": {"elapsed_time_ms": "3", "entries/s": "61330", "iops": "61330", "bytes/s": "251211284", "entries": "200",
                "bytes": "819200", "cpu%": "0"},
 "last_done": {"elapsed_time_ms": "3", "entries/s": "60827", "iops": "60827", "bytes/s": "249148418", "entries": "200",
               "bytes": "819200", "cpu%": "0",
               "latency": {"entries": {"min_us": "23", "avg_us": "30", "max_us": "49",
                                       "percentiles_us": {"1": "24", "50": "31", "75": "34", "99": "48"}},
                           "IO": {"min_us": "7", "avg_us": "9", "max_us": "25",
                                  "percentiles_us": {"1": "8", "50": "10", "75": "11", "99": "21"}}}}}
```

# Live Statistics CSV File

The live statistics csv file (`--livecsv`) gets one row per live statistics interval (`--liveint`) with the totals of all worker threads (standalone mode) or all service instances (distributed mode). With `--livecsvex`, each interval additionally gets one row per worker thread or per service instance. In a read/write mix (`--rwmixpct`, `--rwmixthr`) and in netbench mode, every one of these rows is written twice, once for the write (send) counters and once for the read (receive) counters. The file is appended, so one file can collect the rows of several runs; the header line is only written to an empty file. The special file name `stdout` writes the rows to the console. Every row ends with a trailing comma.

## Fields
* `ISO Date` - timestamp when the row was written
* `Label` - the label, if any, specified for this run on the command line (see `--label`)
* `Phase` - the benchmark phase (READ, WRITE, MKDIRS etc)
* `RuntimeMS` - milliseconds since the start of the phase
* `Rank` - `Total` for the totals row, otherwise the rank of the worker thread (standalone mode) or of the service instance (distributed mode) in the order of the `--hosts` list
* `MixType` - `Write` or `Read` in a read/write mix or netbench phase (the latter meaning send or receive), otherwise empty
* `Done%` - percent of the phase that is done, based on bytes if the phase transfers any, otherwise on entries
* `DoneBytes` - cumulative number of bytes read/written since the start of the phase
* `DoneIOs` - cumulative number of IO operations since the start of the phase
* `MiB/s` - throughput in MiB/s in the interval
* `IOPS` - number of IO operations per second in the interval
* `Entries` - cumulative number of entries (files or directories) done since the start of the phase; 0 if the phase does not count entries
* `Entries/s` - number of entries per second in the interval
* `Lat Ent us` & `Lat Ent max us` - average and maximum time in microseconds to complete an entry in the interval
* `Lat IO us` & `Lat IO max us` - average and maximum time in microseconds to complete an IO operation in the interval; 0 if there was no completed operation in the interval
* `Active` - number of worker threads that are still active; in distributed mode in the totals row the sum over all service instances, in a per-service row the active threads of that service; empty in a per-thread row of standalone mode
* `CPU` - CPU utilization in percent; in distributed mode in the totals row the average over the service instances, in a per-service row the value of that service; empty in a per-thread row of standalone mode
* `Service` - host name of the service instance in a per-service row of distributed mode, otherwise empty
