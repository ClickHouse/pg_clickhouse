#!/bin/sh
# Inventory regression tests to measure output reduction: expected .out files,
# alternates, lines and bytes per test, plus SQL statements and psql
# meta-commands, following \i includes. Run from repository root

set -eu
LC_ALL=C
export LC_ALL

for sql in test/sql/*.sql; do
    name=${sql##*/}
    name=${name%.sql}
    n=0 lines=0 bytes=0
    for out in "test/expected/$name.out" "test/expected/${name}"_[0-9].out; do
        [ -f "$out" ] || continue
        n=$((n + 1))
        lines=$((lines + $(wc -l < "$out")))
        bytes=$((bytes + $(wc -c < "$out")))
    done
    base=0
    [ -f "test/expected/$name.out" ] && base=1
    echo "$name $n $((n - base)) $lines $bytes $base"
done | awk '
    # Count statements outside comments, quotes and COPY data. psql \g
    # variants end statements like semicolons
    function scan(file, dir,    line, n, i, c, w) {
        while ((getline line < file) > 0) {
            if (copying) {
                copying = line != "\\."
                continue
            }
            n = length(line)
            for (i = 1; i <= n; i++) {
                c = substr(line, i, 1)
                if (state == "block") {
                    if (substr(line, i, 2) == "*/") { i++; if (--depth == 0) state = "" }
                    else if (substr(line, i, 2) == "/*") { i++; depth++ }
                } else if (state == "squote") {
                    if (esc && c == "\\") i++
                    else if (c == "\047" && substr(line, i + 1, 1) == "\047") i++
                    else if (c == "\047") state = ""
                } else if (state == "dquote") {
                    if (c == "\"") state = ""
                } else if (state == "dollar") {
                    if (substr(line, i, length(tag)) == tag) { i += length(tag) - 1; state = "" }
                } else if (substr(line, i, 2) == "--") {
                    break
                } else if (substr(line, i, 2) == "/*") {
                    i++; state = "block"; depth = 1
                } else if (c == "\\") {
                    split(substr(line, i + 1), w, " ")
                    if (w[1] ~ /^(g|gx|gset|gexec|gdesc|crosstabview)$/ && pending) end()
                    else meta++
                    if (w[1] ~ /^(i|include)$/) scan(w[2], dir)
                    else if (w[1] ~ /^(ir|include_relative)$/) scan(dir "/" w[2], dir)
                    break
                } else if (c == ";") {
                    if (pending) end()
                } else {
                    if (c == "\047") {
                        state = "squote"
                        esc = substr(line, i - 1, 1) ~ /[Ee]/ && substr(line, i - 2, 1) !~ /[A-Za-z0-9_]/
                    } else if (c == "\"") {
                        state = "dquote"
                    } else if (c == "$" && substr(line, i - 1, 1) !~ /[A-Za-z0-9_]/ && match(substr(line, i), /^\$([A-Za-z_][A-Za-z0-9_]*)?\$/)) {
                        tag = substr(line, i, RLENGTH); i += RLENGTH - 1; state = "dollar"
                    }
                    if (c != " " && c != "\t") pending = 1
                    text = text c
                }
            }
            text = text " "
        }
        close(file)
    }
    function end() {
        stmts++
        copying = toupper(text) ~ /^ *COPY .* FROM +STDIN *$/
        pending = 0
        text = ""
    }
    BEGIN {
        fmt = "%-24s %6s %5s %5s %5s %7s %9s\n"
        printf fmt, "test", "stmts", "meta", "outs", "alts", "lines", "bytes"
    }
    {
        stmts = meta = pending = copying = 0
        state = text = ""
        scan("test/sql/" $1 ".sql", "test/sql")
        printf fmt, $1, stmts, meta, $2, $3, $4, $5
        t[1] += stmts; t[2] += meta; t[3] += $2; t[4] += $3; t[5] += $4; t[6] += $5
        if (!$6) nobase = nobase " " $1
        if (state != "" || copying) print "unterminated " state " in " $1
    }
    END {
        printf fmt, "total " NR, t[1], t[2], t[3], t[4], t[5], t[6]
        if (nobase != "") print "no base .out:" nobase
    }
'

# pg_regress still accepts expected files whose test is gone
for out in test/expected/*.out; do
    stem=${out##*/}
    stem=${stem%.out}
    case $stem in *_[0-9]) stem=${stem%_?} ;; esac
    [ -f "test/sql/$stem.sql" ] || echo "orphan: $out"
done
