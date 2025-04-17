#!/bin/bash

# Generate markdown documents for simple usage instructions based on built-in help text.
# Run this from the root of the repository to generate files in "docs/usage/" based on the
# executable at "bin/elbencho".

DOCS_DIR="docs/usage"
EXE_PATH="bin/elbencho"

if [ ! -d "$DOCS_DIR" ]; then
    echo "ERROR: Docs dir not found. Are you not calling this from the repository root dir?" \
        "Path: $DOCS_DIR" >&2
    exit 1
fi

if [ ! -f "$EXE_PATH" ]; then
    echo "ERROR: Executable not found. Are you not calling this from the repository root dir or" \
        "did you not make the executable before calling this script? Path: $EXE_PATH" >&2
    exit 1
fi

HELP_TOPCIS="$("$EXE_PATH" --help | grep -e "--help-" | grep -o 'help-[a-zA-Z0-9]*' | sort -u)"
if [ -z "$HELP_TOPCIS" ]; then
    echo "ERROR: Discovery of help topics failed." >&2
    exit 1
fi

# turn each help topic into a markdown code block in a separate doc

for i in help $HELP_TOPCIS; do
    echo "Generating docs for: $i"

    echo "## elbencho --$i" > $DOCS_DIR/$i.md # headline

    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to generate doc: $i" >&2
        exit 1
    fi

    echo >> $DOCS_DIR/$i.md

    echo '> **_NOTE:_**  This page has been auto-generated from built-in help text ' \
        'of the `elbencho` executable.' >> $DOCS_DIR/$i.md

    echo >> $DOCS_DIR/$i.md

    # note: we're not using markdown syntax for code blocks here instead of html, because in a
    # markdown code block hyperlinks to other pages would not work.

    echo '<pre><code>' >> $DOCS_DIR/$i.md # start markdown code block
    "$EXE_PATH" --$i >> $DOCS_DIR/$i.md # the actual help content

    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to add help text to doc: $i" >&2
        exit 1
    fi

    echo '</code></pre>' >> $DOCS_DIR/$i.md # end markdown code block
done

# replace help topics in the main help doc with links to .md pages

for i in $HELP_TOPCIS; do
    echo "Adding link to main help page for topic: $i"

    # note: we're using html here instead of markdown link syntax because in a markdown code
    # block hyperlinks to other pages would not work, so this is a html pre-tag block.

    sed -i -e "s! --$i! <a href=\"$i.md\">--$i</a>!g" $DOCS_DIR/help.md

    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to add link to main help page." >&2
        exit 1
    fi
done

# check if number of docs in folder matches number of help topics
# (e.g. to detect stale docs after rename of a help topic)

echo "Checking existing number of docs..."

num_help_topcis="$(echo help $HELP_TOPCIS | wc -w)"

if [ $(ls $DOCS_DIR/*.md | wc -l) -ne $(echo help $HELP_TOPCIS | wc -w) ]; then
    echo "WARNING: Number of '.md' docs in folder does not match number of help topics." \
        "Dir: $DOCS_DIR; Number of topics: $(echo help $HELP_TOPCIS | wc -w)" >&2
fi

echo
echo "All Done."