#!/bin/bash
#
# Test a few simple benchmark cases.
# This requires root privileges to setup a loopback block device for testing.

SCRIPT_PATH=$(dirname $0)
EXE_NAME="elbencho"
EXE_PATH="$SCRIPT_PATH/../bin/$EXE_NAME"
LOOP_BACKING_FILES=("backing1" "backing2")
LOOP_BACKING_FILE_SIZE="$((10*1024*1024))"
LOOPDEV_PATHS=() # array of loop device paths based on LOOP_BACKING_FILES

SKIP_BLOCKDEV_TESTS=0 # user-defined. 1 means skip block device tests.
SKIP_MULTIFILE_TESTS=0 # user-defined. 1 means skip multi-file tests.
SKIP_DISTRIBUTED_TESTS=0 # user-defined. 1 means skip distributed tests.

unset BASE_DIR # user-defined base dir for benchmarks

# Print usage info and exit
usage()
{
  echo "About:"
  echo "  Test a few simple benchmark cases in the given directory, which usually"
  echo "  complete within less than one minute."
  echo "  This requires root privileges to setup a loopback block device for testing."
  echo
  echo "Usage:"
  echo "  $0 [options] <BASEDIR>"
  echo
  echo "Mandatory Arguments:"
  echo "  BASEDIR     Path to a base directory for test files."
  echo
  echo "Optional Arguments:"
  echo "  -b          Skip block device tests. (Device setup requires root privileges.)"
  echo "  -d          Skip distributed tests."
  echo "  -m          Skip multi-file tests."
  echo
  echo "Examples:"
  echo "  Run tests in directory /data/test:"
  echo "    $ $0 /data/test"

  exit 1
}

# Parse command line arguments
parse_args()
{
  local OPTIND # local to prevent effects from other subscripts

  while getopts ":bdmh" opt; do
    case "${opt}" in
      b)
        SKIP_BLOCKDEV_TESTS=1
        ;;
      d)
        SKIP_DISTRIBUTED_TESTS=1
        ;;
      m)
        SKIP_MULTIFILE_TESTS=1
        ;;
      h)
        # help
        usage
        ;;
      *)
        # Other option arguments are invalid
        usage
        ;;
    esac
  done

  shift $((OPTIND-1))

  # 1 here for the mandatory arg: BASEDIR
  if [ $# -ne 1 ]; then
    echo "ERROR: Test directory undefined."
    usage
  fi

  # Non-option arguments are assumed to be the mandatory command line args
  BASE_DIR=$1 # base directory for test files
}

# Check if executable is available and exit if not.
find_executable_or_exit()
{
  if [ ! -f "$EXE_PATH" ]; then
    echo "ERROR: Executable not found. Run \"make\" to create it: $EXE_PATH"
    exit 1
  fi
}

# Check that user-defined basedir exists and exit if not.
check_basedir_or_exit()
{
  if [ ! -d "$BASE_DIR" ]; then
    echo "ERROR: Given path must be an existing directory: $BASE_DIR"
    exit 1
  fi
}

# Prepare loop device for testing.
# Sets LOOPDEV_PATH
prep_loopdev()
{
  for (( i=0; i < ${#LOOP_BACKING_FILES[@]}; i++ )); do
  
    echo "Creating sparse loop device backing file: ${BASE_DIR}/${LOOP_BACKING_FILES[$i]}"
  
    truncate -s ${LOOP_BACKING_FILE_SIZE} ${BASE_DIR}/${LOOP_BACKING_FILES[$i]}
  
    if [ $? -ne 0 ]; then
      echo "ERROR: Creation of loop device backing file failed."
      cleanup_loopdev
      exit 1
    fi
  
    echo "Creating loop device based on file: ${BASE_DIR}/${LOOP_BACKING_FILES[$i]}" 

    LOOPDEV_PATH[$i]=$(sudo losetup --show -f ${BASE_DIR}/${LOOP_BACKING_FILES[$i]})
  
    if [ $? -ne 0 ] || [ -z "${LOOPDEV_PATH[$i]}" ]; then
      echo "ERROR: Creation of loop device failed."
      cleanup_loopdev
      exit 1
    fi
    
    echo "Setting permissions for created loop device: ${LOOPDEV_PATH[$i]}"
    
    sudo chmod o+rw ${LOOPDEV_PATH[$i]}
    
  done
}

# Cleanup created loop device.
cleanup_loopdev()
{
  for (( i=0; i < ${#LOOP_BACKING_FILES[@]}; i++ )); do
  
    if [ -z "${LOOPDEV_PATH[$i]}" ]; then
      continue
    fi
  
    echo "Deleting created loop device: ${LOOPDEV_PATH[$i]}"

    sudo losetup -d ${LOOPDEV_PATH[$i]}
  
    if [ $? -ne 0 ]; then
      echo "ERROR: Deletion of loop device failed."
      exit 1
    fi

    echo "Deleting loop device backing file: ${BASE_DIR}/${LOOP_BACKING_FILES[$i]}"
  
    rm -f  ${BASE_DIR}/${LOOP_BACKING_FILES[$i]}

    if [ $? -ne 0 ]; then
      echo "ERROR: Deletion of loop device backing file failed."
      exit 1
    fi
    
  done
}

# Test 4KiB block random read latency of device ${LOOPDEV_PATH[0]}
test_loopdev_read_lat()
{
  cmd="${EXE_PATH} -r -b 4K --lat --cpu --direct --rand --no0usecerr ${LOOPDEV_PATH[0]}"
  
  echo "Test 4KiB block random read latency of device ${LOOPDEV_PATH[0]}:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_loopdev
    exit 1
  fi
}

# Test 4KiB multi-threaded write IOPS of devices ${LOOPDEV_PATH[0]} & ${LOOPDEV_PATH[1]}
test_loopdev_write_iops()
{
  cmd="${EXE_PATH} -w -b 4K -t 16 --iodepth 16 --direct --rand --no0usecerr "
  cmd+="${LOOPDEV_PATH[0]} ${LOOPDEV_PATH[1]}"
  
  echo "Test 4KiB multi-threaded write IOPS of devices ${LOOPDEV_PATH[0]} & ${LOOPDEV_PATH[1]}:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_loopdev
    exit 1
  fi
}

# Test 1MiB multi-threaded read streaming throughput of device ${LOOPDEV_PATH[0]}
test_loopdev_read_stream()
{
  cmd="${EXE_PATH} -r -b 1M -t 8 --iodepth 4 --direct --no0usecerr ${LOOPDEV_PATH[0]}"
  
  echo "Test 1MiB multi-threaded read streaming throughput of device ${LOOPDEV_PATH[0]}:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_loopdev
    exit 1
  fi
}

# Test 2 threads, each creating 3 directories with 4 1MiB files inside $BASE_DIR
test_multifile_create()
{
  cmd="${EXE_PATH} -t 2 -d -n 3 -w -N 4 -s 1m -b 1m --verify 1 --no0usecerr $BASE_DIR"
  
  echo "Test 2 threads, each creating 3 directories with 4 1MiB files inside $BASE_DIR:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_multifile
    exit 1
  fi
}

# Test 2 threads, each reading 4 1MB files from 3 directories in 128KiB blocks inside $BASE_DIR
test_multifile_read()
{
  cmd="${EXE_PATH} -t 2 -n 3 -r -N 4 -s 1m -b 128k --verify 1 --no0usecerr $BASE_DIR"
  
  echo "Test 2 threads, each creating 3 directories with 4 1MiB files inside $BASE_DIR:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_multifile
    exit 1
  fi
}

# Delete files and directories created by test_multifile_create() inside $BASE_DIR
test_multifile_delete()
{
  cmd="${EXE_PATH} -t 2 -n 3 -N 4 -F -D --no0usecerr $BASE_DIR"
  
  echo "Delete files and directories created by previous test inside $BASE_DIR:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    cleanup_multifile
    exit 1
  fi
}

# Delete files and directories previously created inside $BASE_DIR
cleanup_multifile()
{
  cmd="${EXE_PATH} -t 2 -n 3 -N 4 -F -D --no0usecerr --nodelerr $BASE_DIR"
  
  echo "Cleaning up any files and directories left inside $BASE_DIR:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Cleanup failed."
    exit 1
  fi
}

# Start services.
start_distributed_services()
{
  cmd="${EXE_PATH} --service --zone 0"
  
  echo "Starting local service on default port, bound to NUMA zone 0:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Service start failed."
    exit 1
  fi

  cmd="${EXE_PATH} --service --core 0 --port 1612"
  
  echo "Starting local service on port 1612 without NUMA binding:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Service start failed."
    stop_distributed_services
    exit 1
  fi
}

# Run master to coordinate benchmarks on localhost services, using 4 threads per service and
# creating 8 dirs per thread, each containing 16 1MiB files.
test_distributed_master()
{
  cmd="${EXE_PATH} --hosts localhost,localhost:1612 "
  cmd+="-t 4 -d -n 8 -w -r -N 16 -s 4k -F -D --verify 1 --no0usecerr $BASE_DIR"
  
  echo "Run master to coordinate benchmarks on localhost services, using 4 threads per"
  echo "service and creating 8 dirs per thread, each containing 16 4KiB files:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Test failed."
    stop_distributed_services
    exit 1
  fi
}

# Stop services.
stop_distributed_services()
{
  cmd="${EXE_PATH} --hosts localhost,localhost:1612 --quit"
  
  echo "Stopping local services on default port and on port 1612:"
  echo "  $ ${cmd/"$EXE_PATH"/"$EXE_NAME"}"
  
  $cmd

  if [ $? -ne 0 ]; then
    echo "ERROR: Service termination failed."
    exit 1
  fi
}

parse_args "$@"
find_executable_or_exit
check_basedir_or_exit

if [ $SKIP_BLOCKDEV_TESTS -eq 0 ]; then
  prep_loopdev
  echo
  test_loopdev_read_lat
  echo
  test_loopdev_write_iops
  echo
  test_loopdev_read_stream
  echo
  cleanup_loopdev
fi

echo

if [ $SKIP_MULTIFILE_TESTS -eq 0 ]; then
  test_multifile_create
  echo
  test_multifile_read
  echo
  test_multifile_delete
  echo
  cleanup_multifile
fi

echo

if [ $SKIP_DISTRIBUTED_TESTS -eq 0 ]; then
  start_distributed_services
  echo
  test_distributed_master
  echo
  stop_distributed_services
fi

echo

echo "All done."
