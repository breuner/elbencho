# Using elbencho with Slurm

Elbencho can be used with the Slurm Workload Manager in [interactive mode](#interactive-mode) or in [batch mode](#batch-mode). Interactive mode has the advantage that you can see live statistics during the benchmark.

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

