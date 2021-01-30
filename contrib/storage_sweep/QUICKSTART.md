![Storage sweep logo](pics/storage_sweep.png)

Chin Fang <`fangchin[at]zettar.com`>, Palo Alto, California, U.S.A

<a name="page_top"></a>
Table of Contents
=================

   * [Hello graph_sweep.sh and mtelbencho.sh](#hello-graph_sweepsh-and-mtelbenchosh)
   * [Overview of the two scripts](#overview-of-the-two-scripts)
   * [Design and implementation notes](#design-and-implementation-notes)
   * [Special graphing usage notes](#special-graphing-usage-notes)

# Hello `graph_sweep.sh` and `mtelbencho.sh`

Three steps in the following order:

1. `graph_sweep.sh -h` shows you a few examples. Review a dry-run example. 
2. `mtelbencho.sh -h` to get to know its CLI options.
3. Carry out a `mtelbencho.sh` dry-run based on what you saw
   in 1. Then, adjust the option values to get more acquainted. For
   example, use `-r s -N 1` and (`-v`) to see how its option values
   change
   
Once done, you are ready for the real actions :)

[Back to top](#page_top)

# Overview of the two scripts

The two scripts **`graph_sweep.sh`** and **`mtelbencho.sh`** have been
designed and created such that

1. Both provide concise information for fast learning.  Furthermore,
   when running `$0 -h` or when the dry-run output option (`-n`) is
   used, there is no need to be the `root` or use the `sudo root`
   privilege.
2. With the `graph_sweep.sh`, the verbose mode (`-v`) and the dry-run
   mode (`-n`) show how it carries out a desired multi sweep session.
3. With the `mtelbencho.sh`, the verbose mode (`-v`) and the dry-run
   mode (`-n`) show how it carries out a desired multi test session.
4. The `help()` of both provides actual running and tested examples.
5. A quick way to learn each script is
    1. Starting from **`mtelbencho.sh -h`**, then using the dry-run
       mode to get to know how to run it.
    2. Then, **`graph_sweep.sh -h`**, then using the dry-run mode to
       get acquainted, then carry out actual runs.

[Back to top](#page_top)

# Design and implementation notes

1. Both have been designed to be as compact, readable, and extensible as much
   as possible.
2. The relation among **`graph_sweep.sh`**, **`mtelbencho.sh`**, and
   **`elbencho`** is explained below:
    1. `graph_sweep.sh` is a wrapper of `mtelbencho.sh` and
       *optionally* `gnuplot`
    2. `mtelbencho.sh` is a wrapper of `elbencho`
    3. As such, `graph_sweep.sh` and `mtelbencho.sh` have nearly
       identical options to facilitate learning. Furthermore, whenever
       feasible, their options match `elbencho`'s short options for
       the same purposes, again to facilitate learning.
    4. Both scripts have their defaults carefully chosen so as to make
       running either one as effortless as possible.
3. Both scripts are organized as follows:
    1. Global variables are listed first.
    2. Helper functions for the major functions are next.
    3. Main functions are then listed.
    4. Helper functions for the `# main()` follow the above
    5. Finally, the `# main()` function.
4. The structure of the two scripts makes both amenable to testing,
   verification (with unit tests), and extension.
5. The coding style is a mix of the variable and function naming
   conventions and formatting practices used by C and Perl.  The
   foremost goal is to make the script as readable to humans as
   possible.  So, `bash` idiomatic expressions are not always used so
   as to make each script read like an English prose.
6. Both pass [shellcheck](https://shellcheck.net) without any warnings/errors.
7. Four (4) white spaces are used for indentation throughout (`M-X
   untabify` in GNU Emacs).  Almost all the lines are shorter than 80
   columns.  Almost all functions are shorter than 52 lines (except
   `help()`, `extract_results_for_plotting()`, `run_genuplot()`, and `#
   main()` due to comments or necessity.

[Back to top](#page_top)

# Special graphing usage notes

*Adopted mostly from the embedded comments in* `graph_sweep.sh`:

1. Assuming `elbencho`, `mtelbencho.sh` have been installed, then
  after each run of `graph_sweep.sh`, the script saves in the
  **`output_dir`** defined in the script or provided by the user via
  the `-o` option the following:
    1. The full sweep result files in txt format
    2. Extracted output files named as `p[0-9]`, and a file `plot.dat`
       pasted from them. All in text.
    3. A `sweep.csv` file, which should you wish, can be used as an
       input to another graphing application.
    4. A `sweep.gplt` file, an input file to the `gnuplot` program.
       It can be used to jump-start the learning of the versatile
       `gnuplot` program.
    5. (optionally), if `graph_sweep.sh` is invoked with its `-p`
       (plotting) option and if `gnuplot` is installed on the server,
       then there will be a `sweep.svg` file.
2. The format of the graph is set to `svg`. Nevertheless, it's very
   easy to regenerate a plot based on your preferences. See the
   descriptions for the two global variable `output` and `sweep_gplt`
   in `graph_sweep.sh`.
3. The reason of the choice is that very likely on a headless server,
   there are no TrueType fonts installed, so `svg` is a safer choice.
4. Note that if the gnuplot is used on a server and if its `terminal` 
   is set to `png`, then numerous warnings like the following may appear:
    ```
    gdImageStringFT: Could not find/open font while printing string 1048576x1KiB with font Verdana 
    [...]
    ```
5. Should you prefer to use another graphing application on your
   workstation, such as a spreadsheet, please use the `sweep.csv`
   file in the `output_dir` (defined in the `graph_sweep.sh`).

[Back to top](#page_top)
