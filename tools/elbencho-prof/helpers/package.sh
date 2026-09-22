#!/bin/bash

SCRIPT_NAME=$(basename "$0")
SCRIPT_DIR=$(dirname "$0")
BASE_DIR="$SCRIPT_DIR/.."
BASE_DIR=$(realpath "$BASE_DIR")
HELPERS_DIR="$BASE_DIR/helpers"
ARTIFACTS_DIR="$BASE_DIR/artifacts"

ARCHIVE_BUILD_DIR="builds"
VERSION_FILENAME=".version"


# Print usage info and exit
usage()
{
  echo
  echo "About:"
  echo "  Packages elbencho-prof results into a .tar.gz archive."
  echo
  echo "Usage:"
  echo "  $ $SCRIPT_NAME"
  echo
  echo "Examples:"
  echo " Build local results package:"
  echo " $ $SCRIPT_NAME"
  echo

  exit 1
}

# Parse command line arguments
parse_args()
{
  local OPTIND # local to prevent effects from other subscripts

  while getopts "h" opt; do
    case "${opt}" in
      h)
        # help
        usage
        ;;
      *)
        # Other option arguments are invalid
        echo "ERROR: Invalid argument: ${OPTARG}" >&2
        echo
        usage
        ;;
    esac
  done
}

# Confirm that we are in the root directory of the git repo checkout.
check_repo_root_dir()
{
  local check_filename="elbencho-prof.sh"

  if [ ! -e "$check_filename" ]; then
    echo "ERROR: Important file not found in current directory." \
      "Are you not calling this from the root directory of a git checkout?" \
      "Missing file: $check_filename" >&2
    exit 1
  fi
}

########################### END OF FUNCTION DEFINITIONS ############################

parse_args "$@"

if [ "$(pwd)" != "$BASE_DIR" ]; then
    echo "Changing into base directory: $BASE_DIR"
    cd "$BASE_DIR"
fi

# make sure we are in the right directory
check_repo_root_dir

# version is the short git hash + date
VERSION=$(git log --format=%as-%h -n 1 HEAD 2>/dev/null)

if [ $? -ne 0 ]; then
    VERSION="$(date +%Y%m%d%H%M%S)nogit"
    echo "[INFO] Unable to get version from git log. Using date-based version."
fi

# archive filename includes version string
ARCHIVE_FILENAME="elbencho-prof-$VERSION.tar.gz"

# make sure the build directory exists
mkdir -p "$ARCHIVE_BUILD_DIR"

# if the build is there already for some reason, delete it
if [ -f "$ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME" ]; then
    echo "[INFO] Removing existing build for this release: $ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME"
    rm -f "$ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME"
fi

# create a text file containing the version
echo $VERSION > "$VERSION_FILENAME"

# tar up the contents
# (note: this transforms "." into the dir name "elbencho-prof")
# (note: we don't use "./" for transform regex search because it doesn't match the base dir, which
# must not be included as it would change the permissions of the current dir on extraction.)
if [ $(uname) = "Darwin" ]; then
    # (note: Mac OS tar has "-s" instead of "-transform")
    tar --exclude-vcs -s '!^\.!elbencho-prof!' --exclude artifacts/elbencho \
        --exclude $ARCHIVE_BUILD_DIR -c -z -f "$ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME" .
else
    tar --exclude-vcs --exclude artifacts/elbencho --transform 's!^\.!elbencho-prof!g' \
        --exclude $ARCHIVE_BUILD_DIR -c -z -f "$ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME" .
fi

if [ $? -ne 0 ]; then
    echo "ERROR: Archive creation failed." >&2
    exit 1
fi

# remove the version file
rm -f "$VERSION_FILENAME"

echo "Build available at: $BASE_DIR/$ARCHIVE_BUILD_DIR/$ARCHIVE_FILENAME"

echo
echo "All done."
