#!/bin/bash
#
# Helpers for the journaled data verification tests ("--journaldir").
#
# Source this after lib/testlib.sh:
#   source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1
#
# There is no server to start and no build feature to check, so this library has
# no require_* gate: journaling is always compiled in.
#
# A journal consists of two files per target in the journal dir, a binary file
# holding two bits per journal block and a json sidecar describing it. The
# sidecar is the authority: elbencho finds a journal by scanning every
# "*.journal.json" in the dir and comparing its "target" field against the
# benchmark path *as it was typed on the command line*, and it takes the name of
# the binary file from the sidecar's "binaryFileName" field. So the helpers below
# look journals up by target string and never by file name - the file name
# carries a hash suffix that is deliberately not part of any interface.
#
# Note that boost's json writer quotes every value, so all sidecar fields come
# back as strings. Compare them as strings, i.e. against "4096" and not 4096.

############################ Journal dir contents ###########################

# journal_sidecar_count DIR - number of journals in the journal dir
journal_sidecar_count()
{
    find "$1" -maxdepth 1 -type f -name '*.journal.json' 2>/dev/null | wc -l | tr -d ' '
}

# journal_file_count DIR - number of files in the journal dir
# Has to be twice the sidecar count, because every journal is a sidecar plus a
# binary file. A mismatch means a journal was half created or half removed.
journal_file_count()
{
    find "$1" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' '
}

# journal_targets DIR - sorted, space separated list of all journaled targets
journal_targets()
{
    local dir="$1"

    find "$dir" -maxdepth 1 -type f -name '*.journal.json' 2>/dev/null | \
        sort | xargs -r jq -r '.target' 2>/dev/null | sort | tr '\n' ' ' | sed -e 's/ $//'
}

############################## One journal ##################################

# journal_sidecar_for DIR TARGET
# Path of the sidecar whose "target" field is exactly TARGET, or empty if there
# is none. Prints nothing and returns 1 in that case, so a test can tell "no
# journal for this target" from "the field is empty".
journal_sidecar_for()
{
    local dir="$1"
    local target="$2"
    local sidecar

    for sidecar in "$dir"/*.journal.json; do
        [ -f "$sidecar" ] || continue

        if [ "$(jq -r '.target' "$sidecar" 2>/dev/null)" = "$target" ]; then
            echo "$sidecar"
            return 0
        fi
    done

    return 1
}

# journal_field DIR TARGET FIELD
# One field of that target's sidecar, e.g. journalBlockSize, targetOffset,
# targetSize, journalSeed or binaryFileName. All values are strings.
journal_field()
{
    local sidecar
    sidecar="$(journal_sidecar_for "$1" "$2")" || return 1

    jq -r --arg f "$3" '.[$f] // ""' "$sidecar" 2>/dev/null
}

# journal_binary_for DIR TARGET - path of that target's binary journal file
journal_binary_for()
{
    local name
    name="$(journal_field "$1" "$2" binaryFileName)" || return 1

    [ -n "$name" ] || return 1

    echo "$1/$name"
}

# journal_expected_binary_size SIZE JOURNALBLOCK
# Size that the binary journal of a target of SIZE bytes must have: two bits per
# journal block, packed four per byte, rounded up in both steps.
journal_expected_binary_size()
{
    local numblocks=$(( ($1 + $2 - 1) / $2 ))

    echo $(( (numblocks * 2 + 7) / 8 ))
}

########################### Manipulating a journal ##########################

# journal_remap_target DIR OLDTARGET NEWTARGET
# Point an existing journal at a different target path, which is what the
# sidecar exists for: a renamed device or a moved file keeps its journal.
journal_remap_target()
{
    local sidecar
    sidecar="$(journal_sidecar_for "$1" "$2")" || return 1

    jq --arg t "$3" '.target = $t' "$sidecar" > "$sidecar.new" 2>/dev/null || return 1
    mv "$sidecar.new" "$sidecar"
}

# journal_drop_field DIR TARGET FIELD
# Remove a required field from a sidecar, to check that elbencho rejects it
# instead of reading a journal it cannot interpret.
journal_drop_field()
{
    local sidecar
    sidecar="$(journal_sidecar_for "$1" "$2")" || return 1

    jq --arg f "$3" 'del(.[$f])' "$sidecar" > "$sidecar.new" 2>/dev/null || return 1
    mv "$sidecar.new" "$sidecar"
}

########################### Log message inspection ##########################

# journal_resumed_percent OUTFILE [TARGET]
# The "Verifiable" percentage of the resumed-journal note, e.g. "50.0". With a
# TARGET given, only the note for that target is considered, which is what a run
# over several targets needs. Empty if the run resumed no journal at all.
journal_resumed_percent()
{
    local pattern='Resumed existing journal for target\.'

    if [ -n "${2:-}" ]; then
        pattern="$pattern Target: $(printf '%s' "$2" | sed -e 's/[].[^$*\/]/\\&/g');"
    fi

    grep -oP "$pattern.*Verifiable: \\K[0-9.]+" "$1" 2>/dev/null | head -1
}

############################ Corrupting a target ############################

# corrupt_file_region FILE OFFSET LENGTH
# Overwrite a region of a file with zero bytes, in place and without changing
# the file size, to simulate silent corruption behind elbencho's back.
corrupt_file_region()
{
    dd if=/dev/zero of="$1" bs=1 seek="$2" count="$3" conv=notrunc status=none 2>/dev/null

    return $?
}

# corrupt_file_byte FILE OFFSET
# Flip a single byte, so that a corruption smaller than one journal block is
# covered too.
corrupt_file_byte()
{
    local orig
    orig="$(dd if="$1" bs=1 skip="$2" count=1 status=none 2>/dev/null | od -An -tu1 | tr -d ' ')"

    # write any value other than the one that is there, so this always changes
    # the file no matter what it held before
    if [ "$orig" = "0" ]; then
        printf '\xff' | dd of="$1" bs=1 seek="$2" conv=notrunc status=none 2>/dev/null
    else
        printf '\x00' | dd of="$1" bs=1 seek="$2" conv=notrunc status=none 2>/dev/null
    fi

    return $?
}
