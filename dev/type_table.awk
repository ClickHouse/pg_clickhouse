#!/usr/bin/awk -f
# Filter the pg-clickhouse-c type table into the IMPORT FOREIGN SCHEMA table of
# doc/pg_clickhouse.md. pgch_pg_type_for reports pseudo types no column holds,
# where the import declares text and renders the value through its output
#
# usage: (cd vendor/pg-clickhouse-c && ./gen_type_table.awk) |
#            dev/type_table.awk [markdown-file]

function die(msg) {
    print "type_table: " msg > "/dev/stderr"
    exit (failed = 1)
}

function read_row(got) {
    got = (getline < "/dev/stdin")
    if (got < 0) die("cannot read standard input")
    if (got == 0) return 0
    if ($0 !~ /^\|/) die("expected a table row, got: " $0)
    if (NF != COLS + 2) die("row of " (NF - 2) " cells among rows of " COLS)
    return 1
}

function keep_row(r, c, text) {
    for (c = 1; c <= COLS; c++) {
        cell[r, c] = text = $(c + 1)
        if (length(text) > width[c]) width[c] = length(text)
    }
}

function center(text, c) {
    return sprintf("%" int((width[c] + length(text)) / 2) "s", text)
}

function row(r, c, text, out) {
    out = "|"
    for (c = 1; c <= COLS; c++) {
        text = (r == 0) ? center(cell[r, c], c) : cell[r, c]
        out = out " " sprintf("%-" width[c] "s", text) " |"
    }
    return out
}

function rule(c, dashes, out) {
    out = "|"
    for (c = 1; c <= COLS; c++) {
        dashes = sprintf("%" (width[c] + 2) "s", "")
        gsub(/ /, "-", dashes)
        out = out dashes "|"
    }
    return out
}

BEGIN {
    FS = " *\\| *"
    COLS = 3
    header = "ClickHouse|PostgreSQL|Notes"

    upstream["Map(K,V)"] = "record[]|One record per pair"
    ours["Map(K,V)"] = "text[][]|One row of text items per pair"
    upstream["Nullable(T)"] = "T|Sets nullable on the column"
    ours["Nullable(T)"] = "T|Column imports without NOT NULL"
    upstream["Tuple(...)"] = "record|Pseudo type, no column takes it"
    ours["Tuple(...)"] = "text[]|Fields become text items"

    if (!read_row()) die("no table on standard input")
    if ($2 "|" $3 "|" $4 != header) {
        die("header <" $2 "|" $3 "|" $4 "> is not <" header ">")
    }
    keep_row(0)
    if (!read_row() || $2 !~ /^-+$/) die("no rule under the header")

    while (read_row()) {
        type = $2
        if (type in ours) {
            if ($3 "|" $4 != upstream[type]) {
                die("swap for " type " expects <" upstream[type] ">, " \
                    "got <" $3 "|" $4 ">")
            }
            split(ours[type], swap, "|")
            $3 = swap[1]
            $4 = swap[2]
            swapped[type] = 1
        }
        if ($3 ~ /^record(\[\])*$/) die("no swap for pseudo type row " type)
        keep_row(++rows)
    }
    for (type in ours) {
        if (!(type in swapped)) die("no row for " type)
    }

    table = row(0) "\n" rule()
    for (r = 1; r <= rows; r++) table = table "\n" row(r)

    target = ARGV[1]
    if (target == "") {
        print table
        exit
    }
}

/TYPE-TABLE-BEGIN/ { doc = doc $0 "\n" table "\n"; spliced = 1; skip = 1; next }
/TYPE-TABLE-END/ { skip = 0 }
!skip { doc = doc $0 "\n" }

END {
    if (failed) exit 1
    if (target == "") exit
    if (!spliced || skip) die(target " marks no table to replace")

    printf "%s", doc > target
    close(target)
    print "type_table: " target " updated" > "/dev/stderr"
}
