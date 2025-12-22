#!/bin/bash

# This script updates the copyright and license header in all source files, based on the "reuse"
# tool. Call this from the repository root dir.

SOURCE_DIR="./source/" # dir containing source files to which the copyright header shall be added
REUSE_TEMPLATE_DIR="./.reuse/templates/"
REUSE_TEMPLATE_FILENAME="elbencho-copyright-header.txt"
REUSE_TEMPLATE_PATH="$REUSE_TEMPLATE_DIR/$REUSE_TEMPLATE_FILENAME"
COPYRIGHT_NOTE="2020-2025 Sven Breuner and elbencho contributors"
LICENSE_NOTE="GPL-3.0-only"


# check that the "reuse" tool is available
which "reuse" > /dev/null

if [ $? -ne 0 ]; then
    echo "ERROR: 'reuse' tool not found. Is it installed?" >&2
    exit 1
fi

# check that we're in the root dir of the repo
if [ ! -d "$SOURCE_DIR" ]; then
    echo "ERROR: Source dir not found. Are you not calling this from the repository root dir?" \
        "Path: $SOURCE_DIR" >&2
    exit 1
fi

# create "reuse" format template file
mkdir -p "$REUSE_TEMPLATE_DIR" && \
echo '{{ copyright_lines[0] }}' > "$REUSE_TEMPLATE_PATH" && \
echo 'SPDX-License-Identifier: {{ spdx_expressions[0] }}' >> "$REUSE_TEMPLATE_PATH"

if [ $? -ne 0 ]; then
    echo "ERROR: Failed to create 'reuse' template file." >&2
    exit 1
fi

# add/update copyright header. we feed individual files to "reuse" because otherwise it would
# also add the copyright header to other files like ".gitignore".
find "$SOURCE_DIR" -type f \( -name "*.cpp" -o -name "*.h" \) -exec reuse annotate \
    --exclude-year --single-line --style c --copyright-style spdx \
    --copyright "$COPYRIGHT_NOTE" --license "$LICENSE_NOTE" \
    --template "$REUSE_TEMPLATE_FILENAME" \
    {} +

if [ $? -ne 0 ]; then
    echo "ERROR: Failed to add/update copyright header." >&2
    exit 1
fi

echo "All done."
exit 0
