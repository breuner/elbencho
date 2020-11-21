# Changelog of elbencho

## v1.6.0 (under construction)

### New Features & Enhancements
* New option to truncate files to full size (--trunctosize) in creation/write phase.
* New option to preallocate file disk space (--preallocfile) in creation/write phase.
* File deletion (-F) can be used when benchmark path is a file.

## v1.5.3 (Nov 15, 2020)

### Fixes
* Fixed typos in Makefile related to automatic detection of CUDA path.

## v1.5.1 (Nov 9, 2020)

### New Features & Enhancements
* New support for Nvidia GPUDirect Storage (GDS) via cuFile API (--cufile & --gdsbufreg).
* New bash completion support in deb/rpm packages to complete parameters via tab key.
* Update embedded http server to latest release.
* New option to override service mode GPUs to bind service instances to specific GPUs (--service --gpids ...).
* Show service error messages in preparation phase also on master instead of only in service log.

### Fixes
* Disable phase time limit for sync and cache drop phase, as the low level calls in these phases are blocking.
* Disable phase time limit for worker preparation phase, as a time limit makes no sense in this phase.
* Avoid possible division by 0 with enable live stats in sync and cache drop phase.
* Properly release GPU resources at end of benchmark phase in service mode.
* Update automatic path detection during make for GDS v0.9 beta release.

## v1.4-3 (Oct 30, 2020)

### New Features & Enhancements
* Automatically detect availability of CUDA development libs on system and enable support. (Manually enable/disable still possible via Makefile options.)
* New `--version` parameter to show executable version, network protocol version and included optional build features (such as CUDA support).
* Move boost.org dependencies to separate Makefile variables (CXXFLAGS_BOOST, LDFLAGS_BOOST) to make it easier for users of custom boost libs.

### Fixes
* Properly release service host file descriptors and libaio context if benchmark was interrupted by I/O error or ctrl+c.
* Properly update cpu usage info during single line live stats.
* Do not return application error code when phase time limit exceeded.

### Contributors
* Thanks to first time contributors Chin Fang (Zettar) & team, Michael Chichik (Excelero), Marco Cicala (E4), Glenn Lockwood (NERSC/LBNL) for helpful comments.

## v1.4-1 (Oct 12, 2020)

### New Features & Enhancements
* New phase to sync write buffers to stable storage (--sync).
* New phase to drop linux kernel page cache, dentry cache and inode cache (--dropcache).
* New phase to stat files (--stat).
* New option to read service hosts from file (--hostsfile).
* New CPU utilization info in live stats and phase results (--cpu).
* New option to configure service host update retrieval interval (--svcupint).
* Print CSV headers only to first line of CSV file.
* Reorder CSV file columns for better readability and add CPU utilization info.

### Contributors
* Thanks to first time contributors Salvador Martin (HPCNow), Joe Harlan (Excelero), Wolfgang Szoecs (HPE) for helpful comments.

## v1.3 (Sep 8, 2020)

### New Features & Enhancements
* Improved console output to be easier to read
* Add more accurate dependencies to .deb package
* Add user name to service mode log file name to prevent conflicts between different users
* Update embedded http server to latest release
* Use given `--port` value also in master mode
* Increased internal elapsed time resolution from milli to microseconds

## v1.2 (Aug 17, 2020)

### New Features & Enhancements
* Added data integrity checks.

### Contributors
* Thanks to first time contributor Kirill Shoikhet (Excelero) for helpful comments.

## v1.1 (Aug 12, 2020)

### New Features & Enhancements
* Added GPU data transfer support via Nvidia CUDA library.

### Contributors
* Thanks to first time contributor Eliott Kespi (Excelero).

## v1.0 (July 8, 2020)
* Initial release by Sven Breuner.
