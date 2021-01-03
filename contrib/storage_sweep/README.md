## December 25, 2020

![Storage sweep logo](pics/storage_sweep.png)

Chin Fang <`fangchin[at]zettar.com`>, Palo Alto, California, U.S.A

# Table of contents

1. **Introduction**
2. **Requirements**
3. **Layout and content**
4. **Goals and possible uses**
    1. **Primary goal of each wrapper and the main use**
	2. **Other uses**
5. **Motivation**
6. **A brief overview of storage benchmarking**
    1. **A typical goal**
	2. **How it should be done**
	3. **A common misconception**
	4. **Carry out you own storage benchmarking**
7. **Future evolution**
8. **Acknowledgments**
9. **Epilogue**

# Introduction

This directory contains two `bash` scripts:

1. **`graph_sweep.sh`** is a wrapper script of `mtelbencho.sh` and
   [`gnuplot`](http://www.gnuplot.info).  Its main purpose is to make
   the graphing of storage sweeps a push-button operation as much as
   possible.  Once it's installed, type `graph_sweep.sh -h` for more
   info.
2. **`mtelbench.sh`** (mt: multiple test) is a wrapper script for
   `elbencho`. Once it's installed, type `mtelbencho.sh -h` for more
   info.

`elbencho` is a modern, distributed, fast, storage space efficient,
and easy to use storage benchmarking application. There is no need to
study many pages of documentation or search the Internet up and down
looking for tutorials, hoping to get some useful tips.  Merely run:

`elbencho --help-all | less` 

and read carefully. With a bit of practice, one should acquire enough
proficiency to use `elbencho` for real work.  This is how I learned
`elbencho`.

Nevertheless, it is helpful to see how this versatile program is used
in real production/test environment.  `mtelbencho.sh` provides a
simple, production grade, useful, and extensible example.
`graph_sweep.sh` enables you to graph the obtained results in a push
button manner.

Before proceeding, you may wish to browse the
**[`QUICKSTART.md`](QUICKSTART.md)** first.

# Requirements

1. There must be two TB (1TB=2000000000000 bytes) available to conduct
   a sweep.  Note that only hyperscale datasets are used, where the
   term "hyperscale dataset" is defined as a dataset that has overall
   size >= 1TB (terabyte), or contains >= 1 million files, or both.
2. `elbencho` must be installed and available in the `root`'s `$PATH`.
3. Both `mtelbencho.sh` and `graph_sweep.sh` must be install in
   a directory that is part of the `root`'s `$PATH`. The key reason
   is that `mtelbencho.sh` runs `elbencho` with its `--dropcache`
   option, which demands `root` privilege.

# Layout and content

This directory contains 

1. `graph_sweep.sh` 
2. `mtelbencho.sh` 
3. A '`sw_tests`' subdirectory.  Please review the
   [`README.md`](sw_tests/README.md) in this subdirectory.

# Goals and possible uses

## Primary goal of each wrapper and the main use

### mtelbencho.sh 
A storage sweep (*described more in the **Motivation** and **A brief
overview of storage benchmarking** sections below*) is a simple and
effective way to learn about *the current* performance and
characteristics of a storage service. Such a sweep should be carried
out by any IT professional responsible for an organization's storage,
especially a new deployment.

The foremost goal of `mtelbencho.sh` is to simplify the already easy
`elbencho` usage even more for carrying out a storage sweep over a
wide range of file sizes. As implemented, it does so by default
sweeping from **1KiB** to **1TiB**, incremented in power-of-two file
sizes.  Once installed, please type `$ mtelbencho.sh -h` on the
command line for more details.  We also recommend you consulting the
[`QUICKSTART.md`](QUICKSTART.md).

### graph_sweep.sh

Nevertheless, often times it is much desirable to have the numerical
results plotted in simple to understand form. This is where the
`graph_sweep.sh` comes in. Please see figures below, produced with the
`graph_sweep.sh`'s `-p` (*push button plotting option*). You may wish
to consult the [`QUICKSTART.md`](QUICKSTART.md) again.

![LOSF 3-run sweep graph](pics/s_sweep.svg)
![Medium file 3-run sweep graph](pics/m_sweep.svg)
![Large file 3-run sweep graph](pics/l_sweep.svg)
![Overall 3-run sweep graph](pics/o_sweep.svg)

Note that due to the gradual changes of the performance
characteristics of SSDs, the shape of the graphs above may change over
time.  More below in the next subsection.

## Other uses

Using the two wrappers and `elbencho`, one can conveniently accomplish
the following:

1. Gain a good understanding of a file storage system in
   service.
2. Pick the most performant candidate from several distributed,
   scale-out file storage service candidates.
3. Select the most appropriate storage devices, from multiple
   choices, for a given file storage service.
5. Prepare (aka condition) a large number of new SSDs using the two
   wrappers together with `elbencho`, recalling it's a distributed
   application.
6. Monitor the characteristics of a batch of SSDs in production over
   time, either visually or numerically.  Note that SSDs, except
   [Intel
   Optane](https://www.intel.com/content/www/us/en/architecture-and-technology/optane-technology/optane-enterprise-storage.html),
   tend to change their performance characteristics over time.  Please
   see [The Why and How of SSD Performance
   Benchmarking](https://www.snia.org/educational-library/why-and-how-ssd-performance-benchmarking-2011)
   for a more in-depth discussion.

Many other uses are possible. It's up to one's creativity and
imagination.

# Motivation

My professional focus and specialty is moving data at scale and when
feasible, at speed.  Since 2015, my company, [Zettar
Inc.](https://zettar.com/) has been engaged to support the ambitious
data movement requirements of [Linac Coherent Light Source II
(LCLS-II)](https://lcls.slac.stanford.edu/lcls-ii/design-and-performance),
a premier U.S. Department of Energy (DOE)'s Exascale Computing
preparation project.  As a result, my team and I have become
intimately familiar with the DOE's co-design principle: *integrated
consideration of storage, computing, networking, and highly
concurrent, scale-out data mover software for optimal data movement
performance*.

We also learned that when undertaking any serious moving data at scale
and speed effort, *the first step* should always be gaining a good
understanding of the storage system's I/O throughput and
characteristics, typically by storage benchmarking.

While collaborating with the DOE's Energy Sciences Network (ESnet) in
2020, documented in [Zettar zx Evaluation for ESnet
DTNs](https://www.es.net/assets/Uploads/zettar-zx-dtn-report.pdf), one
challenge was the file size range needed to be considered -- there was
simply not a single storage benchmarking tool that we knew of could
meet our needs.  The timely appearance of `elbencho` was really a
serendipity as it met our requirements perfectly.  As a result, the
ESnet/Zettar project concluded both timely and successfully.

Believing what we learned and did during the project could be helpful
to others, I decided to rewrite and consolidate the various test
scripts that I wrote for the project into a pair of `bash` wrappers. 

# A brief overview of storage benchmarking

The views and opinions expressed below are for serious data processing
tasks. Also, they reflect my background in high-performance computing
(HPC) at the U.S. DOE national lab level.  Nevertheless, I anticipate
the information to be useful to large distributed data-intensive
enterprises.

## A typical goal

It is to understand the impact of storage's I/O throughput and
characteristics to one or more selected target application(s).

## How it should be done

1. If the target application is a host-oriented one, such as a typical
   database application, then the storage benchmarking should be
   carried out on a single **storage client host** (*identical in
   hardware spec and configuration to the one that **is** intended to run
   said application*).
2. If the target application is a cluster application running on e.g.
   5 cluster nodes, then the storage benchmarking should be carried
   out concurrently on five **storage client hosts** (*identical in
   hardware spec and configuration to the ones that **are** intended to
   run said application*).
3. In general, such benchmarking should be carried out over storage
   interconnects (e.g. InfiniBand or Ethernet (RoCE likely
   configured)).

## A common misconception

Often we read that a storage service can provide, e.g. 1PB/s
throughput :grin:.

This number is meaningless because the lack of info about the
following, among the others:

1. How was the number obtained? What is the benchmark hardware setup?
   How about the benchmark software?
2. How is the benchmark software configured? 
3. What kind of datasets was used for the measurements? e.g. their
   respective histogram?
4. How long and how many times did the benchmark take to run?
5. What is the interconnect connecting the storage clients for
   benchmarking and the storage service?
6. What is the hardware spec of the storage clients running the
   benchmark? Their tuning level?
7. Is a production setup used or a purposely constructed one for show
   only?
  
## Carry out your own storage benchmarking

So, we recommend using `graph_sweep.sh` and `mtelbencho.sh` to carry
out storage sweeps in your environment and obtain your own numbers,
which you can trust.  `elbencho` together with the two bash wrappers
empower you to do so conveniently.
  
# Future evolution

`elbencho` is a distributed storage benchmark for file systems and
block devices with support for GPUs.  It is far more capable than the
two wrappers cover.  In addition, they don't directly handle the
benchmarking of scale-out storage.  Although coupled with something
like [Ansible](https://www.ansible.com) and `elbencho`'s service mode
(see `elbencho --help-all`), it can be done.  Future improvement and
evolution are definitely possible.

# Acknowledgments

* **Sven Breuner**, for creating the excellent `elbencho` and
  enhancing it rapidly, his friendship, and many stimulating
  discussions over the years.
* **Sandy Wambold**, for her friendship and English help.  If the
  content of this README.md is readable, please credit her.
* **Igor Soloviov**, for his patient help any time I become rusted in
  software development skills.
* **Oleskandr Nazarenko**, for his code reviews and helpful critiques.

# Epilogue

I hope you enjoy using the two modest wrappers as much as I did creating
them!
