## December 25, 2020

# Table of contents

1. **Introduction**
2. **Layout and content**
3. **Goals and possible use of the wrapper**
4. **Motivation**
5. **Future evolution**
6. **Acknowledgments**
7. **Epilogue**

## Introduction

`mtelbench.sh` is a wrapper script for `elbencho`.

`elbencho` is a modern, fast, storage space efficient, and easy to use
storage benchmarking application. There is no need to study many pages
of documentation or search the Internet up and down looking for
tutorials, hoping to get some useful tips.  Merely run:

`elbencho --help-all | less` 

and read carefully. With a bit of practice, one should acquire enough
proficiency to use elbencho for real work.  This is how I learned
`elbencho`.

Nevertheless, it is helpful to see how this versatile program is used
in real production/test environment. This `bash` wrapper provides a
comprehensive, production grade, and yet simple example.

## Layout and content

This directory contains 

* `mtelbencho.sh` (mt: multiple test), a simple bash wrapper for `elbencho`.
* A '`tests`' subdirectory.  Please review the `README.md` in the '`tests`'
  subdirectory.

## Goals and a possible use

A storage sweep (described more in the **Motivation** section below) is a
simple and effective way to learn about the performance and
characteristics of a storage service. Such a sweep should be carried
out by any IT professional responsible for an organization's storage,
especially for new deployment.

The foremost goal of this wrapper is to simplify the already simple
`elbencho` usage even more for carrying out a storage sweep over a wide
range of file sizes. As implemented, it does so by default sweeping
from 1KiB to 1TiB, incremented in power-of-two file sizes.  Once
installed, please type `$ mtelbencho.sh -h` on the command line for more
details.

Using this wrapper, one can quickly and simply accomplish the
following:

1. Gain a good understanding of a storage system in
   service.

2. Pick the performant candidate from serveral distributed,
   scale-out storage service candidates,
   
3. Picking the most appropriate storage devices, from multiple
   choices, for a given storage service.

Many other uses are possible. It's up to one's creativity and
imagination.

## Motivation

My professional focus and speciality is moving data at scale and when
feasible, at speed.  Since 2015, my company, [Zettar
Inc.](https://zettar.com/) has been engaged to support the ambitious
data movement requirements of [Linac Coherent Light Source II (LCLS-II)](https://lcls.slac.stanford.edu/lcls-ii/design-and-performance), a
premier U.S. Department of Energy's Exascale Computing preparation
project.  As a result, my team and I have become intimately familar
with the DOE's co-design principle: *integrated consideration of
storage, computing, networking, and highly concurrent, scale-out data
mover software for optimal data movement performance*.

We also learned that when undertaking any serious moving data at scale
and speed effort, *the first step* should always be to gain a good
understanding of the storage system's I/O throughput and
characteristics, typically by storage benchmarking.

While collaborating with the DOE's Energy Sciences Network (ESnet) in
2020, documented in [Zettar zx Evaluation for ESnet DTNs](https://www.es.net/assets/Uploads/zettar-zx-dtn-report.pdf), one
challenge was the file size range needed to be considered -- there was
simply not a single storage benchmarking tool that we knew of could
meet our needs.  The timely appearance of `elbencho` was really
a serendipity as it met our requirements perfectly.  As a result, the
ESnet/Zettar project concluded both timely and successfully.

Believing what we learned and did during the project could be helpful
to others, I decided to rewrite and consolidate the various test
scripts that I wrote for the project into a single bash wrapper. 

## Future evolution

`elbencho` is a distributed storage benchmark for file systems and block
devices with support for GPUs.  It is far more capable than the
wrapper covers.  In addition, the wrapper by itself doesn't handle the
benchmarking of scale-out storage.  Although coupled with something
like Ansible, it can be done.  Future improvment and evolution
definitely are possible.

## Acknowledgments

* **Sven Breuner** <`sven.breuner[at]gmail.com`> for creating the excellent 
  `elbencho` and enhancing it rapidly, his friendship, and many stimulating 
  discussions over the years.
  
* **Sandy Wambold** <`sandy[at]wambold.com`> for her friendship and English
  help.  If the content of this README.md is readable, please credit
  her.

## Epilogue

I hope you enjoy using this modest wrapper as much as I did creating
it!

Chin Fang <`fangchin[at]zettar.com`>, Palo Alto, California, U.S.A
