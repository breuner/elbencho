# Changelog of elbencho

## v1.4 (TBD)

### New Features & Enhancements
* Added new phase to sync write buffers to stable storage (--sync).
* Added new phase to drop linux kernel page cache, dentry cache and inode cache (--dropcache).
* Added new phase stat files (--stat).
* Print CSV headers only to first line of CSV file.
* Added option to read service hosts from file (--hostsfile)

### Contributors
* Thanks to first time contributor Salvador Martin (HPCNow) for helpful comments.

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