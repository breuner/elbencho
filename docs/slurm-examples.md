# Using elbencho with Slurm

Elbencho can be used with the Slurm Workload Manager in [interactive mode](#interactive-mode) or in [batch mode](#batch-mode). Interactive mode has the advantage that you can see live statistics during the benchmark. The MPI-IO mode runs as a regular MPI job, see [batch mode with MPI-IO](#batch-mode-with-mpi-io).

> **_NOTE:_** For the examples below, we assume that the elbencho rpm or deb package is installed on all nodes, so that we don't need to provide a path to elbencho. Alternatively, you could use the static build in tar format from the [releases section](https://github.com/breuner/elbencho/releases) and run it from a shared directory or use the `sbcast` command to transfer the executable from your job submission node to all allocated nodes of a job.

> **_NOTE:_** Thanks to Glenn Lockwood for providing the original example [here](https://www.glennklockwood.com/benchmarks/elbencho.html) on which this guide is based.

## Interactive Mode

In this example, we use an interactive Slurm session on 3 nodes to write a single shared 10GiB file from all 3 nodes, assuming `/mnt/shared` is a shared filesystem mountpoint on all 3 nodes.

First, let's get an exclusive interactive session on 3 nodes:

```bash
srun --nodes=3 --ntasks-per-node=1 --exclusive --cpu-bind=none --pty bash -i
```

Inside the interactive session, use  `srun` to start elbencho in service mode on all allocated nodes:

```bash
srun elbencho --service --foreground &
```

Now get the node list in a format that is compatible with elbencho:

```bash
HOSTNAMES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr "\n" ",")
```

And with that we're ready to start the actual benchmark to write our shared 10GiB file:

```bash
elbencho --hosts "$HOSTNAMES" -w -s 10g -b 1m -t 4 --direct /mnt/shared/testfile
```

After this, we could either submit more benchmarks to the running elbencho service instances or decide that we're done and stop the service instances:

```bash
elbencho --hosts "$HOSTNAMES" --quit
```

## Batch Mode

In this example, we use a Slurm batch job to write a 10GiB file from 3 nodes to a shared filesystem mounted at `/mnt/shared`.

For batch mode, we need a script to submit via Slurm's `sbatch` command. This script needs to prepare the elbencho service mode instances, run the actual benchmark afterwards and finally stop the service instances.

Let's start by creating the Slurm job script for our batch submission. The content of this script is mostly similar to the steps that we ran in the interactive session example above - one small exception being that we specify an elbencho result file (`--resfile`) instead of console output, as console output lines might get reordered.

We'll call our new Slurm job script `elbencho-batch-job.sh`:

```bash
# Build elbencho compatible list of hostnames
# (Comma-separation instead newline not required, but looks nicer.)
HOSTNAMES=$(scontrol show hostnames "$SLURM_JOB_NODELIST" | tr "\n" ",")

# Start service on all nodes
srun elbencho --service --foreground &

# Run benchmark

echo "Starting benchmark... ($(date))"

elbencho --hosts "$HOSTNAMES" --svcwait 10 --resfile elbencho-result.txt -w -s 10g -b 1m -t 4 --direct /mnt/shared/testfile

echo "Benchmark finished. ($(date))"

# Here we could either submit more benchmarks to the running service
# instances or decide that we're done and quit the services.

# Quit services
elbencho --quit --hosts "$HOSTNAMES"
```

That's it. Now we just need to submit our job script to the batch queue and can afterwards check the `elbencho-result.txt` file for the result:

```bash
sbatch --nodes=3 --ntasks-per-node=1 --exclusive --cpu-bind=none --job-name=elbencho elbencho-batch-job.sh
```

## Batch Mode with MPI-IO

The MPI-IO mode ([docs/mpiio.md](mpiio.md)) runs elbencho as an MPI job. In this example, 4 nodes
write and read a 32GiB shared file on `/mnt/shared` with collective I/O. We use 8 I/O ranks per
node with one thread each, which works with every MPI library (see the note on threads and
collective I/O in [docs/mpiio.md](mpiio.md)). `srun` launches the MPI job here, which requires
Slurm to be set up for the MPI library (e.g. `srun --mpi=pmix`); alternatively, use the MPI
library's own `mpirun` inside the job script.

> **_NOTE:_** The MPI-IO mode requires an elbencho executable built with `MPI_SUPPORT=1` against
> the MPI library of the cluster (see [docs/mpiio.md](mpiio.md)), so the packaged builds don't work
> here. `elbencho --version` shows `mpiio` in the included features if the build has it.

**Variant 1: one MPI job.** With `--mpiio` on the command line, rank 0 becomes the coordinator and
all other ranks become the I/O ranks, so no separate service instances are needed. The job script
`elbencho-mpiio-job.sh` is a single line:

```bash
srun elbencho --mpiio --mpicoll --strided --resfile elbencho-result.txt -w -r -s 32g -b 1m -t 1 /mnt/shared/testfile
```

We submit it with one task more than the 32 I/O ranks due to the coordinator rank 0, which runs
additionally on the first node. (Thus, two `sbatch` allocation packs separated by `:`, where 1st
node has one additional task.) The coordinator rank only coordinates, so sharing the first node with
the 8 I/O ranks is fine:

```bash
NUM_TASKS_PER_NODE=8 # not including extra coordinator rank on 1st node
NUM_NODES=4

sbatch --job-name=elbencho --cpu-bind=none --nodes=1 --ntasks=$((NUM_TASKS_PER_NODE + 1)) --exclusive $( [[ $NUM_NODES -gt 1 ]] && echo ": --nodes=$((NUM_NODES - 1)) --ntasks-per-node=$NUM_TASKS_PER_NODE --exclusive" ) elbencho-mpiio-job.sh
```

**Variant 2: service ranks.** Like the batch mode example above, the MPI job can also start
persistent service instances, against which the job script then runs any number of benchmarks. All
MPI ranks do I/O in this variant. The services pick their ports and register themselves in a hosts
file on the shared file system, which the benchmark runs then use. The job script
`elbencho-mpiio-services-job.sh`:

```bash
HOSTSFILE=/mnt/shared/elbencho-hosts-$SLURM_JOB_ID

# Start MPI-IO service ranks on all nodes (8 per node, see sbatch below)
rm -f "$HOSTSFILE"
srun elbencho --service --mpiio --foreground --port 0 --hostsfile "$HOSTSFILE" &

# Wait until all services registered themselves
while [ "$(grep -c . "$HOSTSFILE" 2>/dev/null)" != "$SLURM_NTASKS" ]; do sleep 1; done

# Run benchmarks: 32 I/O ranks with one thread each
elbencho --mpiio --mpicoll --strided --hostsfile "$HOSTSFILE" --resfile elbencho-result.txt -w -r -s 32g -b 1m -t 1 /mnt/shared/testfile

# Quit services
elbencho --quit --hostsfile "$HOSTSFILE"
```

```bash
sbatch --nodes=4 --ntasks-per-node=8 --exclusive --cpu-bind=none --job-name=elbencho elbencho-mpiio-services-job.sh
```
