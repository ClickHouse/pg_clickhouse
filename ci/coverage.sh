#!/bin/sh
# Line coverage of `make COVERAGE=1` builds, per CI lane and merged
#
# usage: ci/coverage.sh prepare
#        ci/coverage.sh stop
#        ci/coverage.sh collect OUTDIR "SOURCE..."
#        ci/coverage.sh report LCOV OUTDIR "SOURCE..."
#        ci/coverage.sh merge OUTDIR LANESDIR LANE...
#
# prepare and stop drive Debian cluster from pgxn-tools pg-start, as root.
# collect, report and merge run from repository root

set -eu
LC_ALL=C
export LC_ALL

die() {
    echo "coverage: $*" >&2
    exit 1
}

# pg_lsclusters columns: version cluster port status owner datadir logfile.
# Package install creates stopped main cluster beside pg-start's test cluster
cluster() {
    pg_lsclusters -h | awk '$4 == "online" { print; exit }'
}

prepare() {
    : "${GCOV_PREFIX:?set to an absolute directory postgres may create}"
    read -r ver name _ <<EOF
$(cluster)
EOF
    # pg-build-test runs `sudo make install`, sudo resets environment, and
    # install without COVERAGE adds uninstrumented bitcode for JIT inlining
    echo 'Defaults env_keep += "COVERAGE"' > /etc/sudoers.d/coverage
    # Backends run as postgres, unable to create .gcda in root's build tree.
    # libgcov prepends GCOV_PREFIX to absolute object path and creates missing
    # dirs (GCC manual, Data File Relocation to Support Cross-Profiling).
    # pg_ctlcluster gives postgres only this file's environment
    echo "GCOV_PREFIX = '$GCOV_PREFIX'" >> "/etc/postgresql/$ver/$name/environment"
    pg_ctlcluster "$ver" "$name" restart
}

stop() {
    read -r ver name _ _ _ _ log <<EOF
$(cluster)
EOF
    # Backends write counters from exit() handlers. Fast mode SIGTERMs them
    # into proc_exit(), then exit(). Immediate shutdown and crash recovery
    # SIGQUIT them into quickdie(), whose _exit() drops counters
    pg_ctlcluster "$ver" "$name" stop -m fast
    # libgcov reports unwritable counters on stderr, which lands in server log
    if grep -E '^profiling:|was terminated by signal|terminating any other active server processes' "$log"; then
        die "$log shows lost or unwritable counters"
    fi
}

# Read .gcno and .gcda with gcov from same GCC release as $CC
gcov_tool() {
    gcov=$(echo "${CC:-gcc}" | sed 's/gcc/gcov/')
    want=$("${CC:-gcc}" -dumpfullversion)
    have=$("$gcov" --version | head -n 1)
    [ "${have##* }" = "$want" ] || die "$gcov reports $have, ${CC:-gcc} is $want"
    echo "$gcov"
}

collect() {
    out=$1
    root=$(pwd -P)
    gcov=$(gcov_tool)
    mkdir -p "$out"
    {
        echo "revision=$(git rev-parse HEAD)"
        echo "lane=${LANE:-local}"
        echo "runs=${RUNS:-unknown}"
        echo "pg=$("${PG_CONFIG:-pg_config}" --version)"
        echo "ch=$(clickhouse-server --version 2>/dev/null || echo unknown)"
        echo "cc=$("${CC:-gcc}" --version | head -n 1)"
        echo "gcov=$("$gcov" --version | head -n 1)"
        echo "sources=$2"
    } > "$out/meta.txt"

    # Relocated counters mirror absolute object paths, GCOV_PREFIX_STRIP unset
    if [ -n "${GCOV_PREFIX:-}" ] && [ -d "$GCOV_PREFIX$root/src" ]; then
        cp -R "$GCOV_PREFIX$root/src/." src/
    fi

    # Initial capture lists every instrumented line at zero, so objects never
    # run still count as missed
    lcov --quiet --gcov-tool "$gcov" --capture --initial --directory src --output-file "$out/base.info"
    if [ -n "$(find src -name '*.gcda')" ]; then
        lcov --quiet --gcov-tool "$gcov" --capture --directory src --output-file "$out/run.info"
        lcov --quiet --add-tracefile "$out/base.info" --add-tracefile "$out/run.info" --output-file "$out/all.info"
    else
        echo "coverage: no .gcda counters under src" >&2
        cp "$out/base.info" "$out/all.info"
    fi

    # Drop job-specific root so lanes merge by repository path
    awk -v root="$root/" 'index($0, "SF:" root) == 1 { $0 = "SF:" substr($0, length(root) + 4) } 1' \
        "$out/all.info" > "$out/lcov.info"
    rm -f "$out/base.info" "$out/run.info" "$out/all.info"
    status=0
    report "$out/lcov.info" "$out" "$2" || status=1

    # Installed module must be this build, with no bitcode for JIT to inline
    lib=$("${PG_CONFIG:-pg_config}" --pkglibdir)
    cmp pg_clickhouse.so "$lib/pg_clickhouse.so" || status=1
    if [ -e "$lib/bitcode/pg_clickhouse.index.bc" ]; then
        echo "coverage: $lib/bitcode has uninstrumented pg_clickhouse" >&2
        status=1
    fi
    return $status
}

# Summarize line coverage by file. src/ holds extension code, vendor/ holds
# vendored headers ignored in reports, absolute paths are system and
# PostgreSQL headers. Only src/ counts toward extension audits and
# missed-line ceiling
report() {
    mkdir -p "$2"
    awk -v out="$2" -v sources="$3" -v max="${MAX_MISSED_LINES:-}" '
        function bucket(f) {
            return f ~ /^src\// ? "src" : "external"
        }
        function fail(msg) {
            print "coverage: " msg > "/dev/stderr"
            failed = 1
        }
        BEGIN {
            uncovered = "sort -t: -k1,1 -k2,2n > " out "/uncovered.txt"
            rows = "sort -k1,1 -k5 > " out "/summary.txt"
            totals = out "/totals.txt"
            printf "" > (out "/uncovered.txt")
            close(out "/uncovered.txt")
        }
        /^SF:/ { sf = substr($0, 4); files[sf] }
        /^DA:/ {
            split(substr($0, 4), d, ",")
            if (!((sf, d[1]) in count)) lf[sf]++
            count[sf, d[1]] += d[2]
        }
        END {
            for (k in count) {
                split(k, p, SUBSEP)
                if (count[k] > 0) lh[p[1]]++
                else if (bucket(p[1]) != "external") print p[1] ":" p[2] | uncovered
            }
            close(uncovered)
            for (f in files) {
                b = bucket(f)
                tf[b] += lf[f]
                th[b] += lh[f]
                printf "%-8s %7d %7d %7d  %s\n", b, lf[f], lh[f], lf[f] - lh[f], f | rows
                if (b != "external" && (getline line < f) < 0) fail(f " not in checkout")
                close(f)
                if (b == "src" && lf[f] > 0 && lh[f] == 0) fail(f " has " lf[f] " lines, none hit")
            }
            close(rows)
            n = split(sources, s, " ")
            for (i = 1; i <= n; i++)
                if (!lf[s[i]]) fail(s[i] " compiled but missing from report")
            if (!tf["src"]) fail("empty src/ inventory")
            hdr = sprintf("%-8s %7s %7s %7s", "bucket", "lines", "hit", "missed")
            print hdr > totals
            print hdr
            split("src external", order, " ")
            for (i = 1; i <= 2; i++) {
                b = order[i]
                if (!tf[b]) continue
                row = sprintf("%-8s %7d %7d %7d  %.2f%%", b, tf[b], th[b], tf[b] - th[b], 100 * th[b] / tf[b])
                print row > totals
                print row
            }
            close(totals)
            if (max != "" && tf["src"] - th["src"] > max + 0)
                fail(tf["src"] - th["src"] " missed src/ lines exceed ceiling " max)
            exit failed
        }
    ' "$1"
}

# Value of KEY in lane metadata FILE
meta() {
    sed -n "s/^$1=//p" "$2"
}

merge() {
    out=$1
    lanes=$2
    shift 2
    incomplete=
    for lane; do
        if [ ! -f "$lanes/$lane/meta.txt" ] || [ ! -s "$lanes/$lane/lcov.info" ]; then
            echo "coverage: lane $lane lacks meta.txt or lcov.info" >&2
            incomplete=1
        fi
    done
    for dir in "$lanes"/*; do
        [ -e "$dir" ] || continue
        case " $* " in
            *" ${dir##*/} "*) ;;
            *) echo "coverage: unexpected lane ${dir##*/}" >&2 && incomplete=1 ;;
        esac
    done
    [ -z "$incomplete" ] || die "lane coverage incomplete"

    status=0
    head=$(git rev-parse HEAD)
    first=$lanes/$1/meta.txt
    mkdir -p "$out/lanes"
    : > "$out/lanes.txt"
    for lane; do
        m=$lanes/$lane/meta.txt
        [ "$(meta revision "$m")" = "$head" ] || { echo "coverage: lane $lane built another revision" >&2 && status=1; }
        for key in cc gcov sources; do
            [ "$(meta "$key" "$m")" = "$(meta "$key" "$first")" ] || { echo "coverage: lane $lane $key differs from $1" >&2 && status=1; }
        done
        for run in $(meta runs "$m"); do
            [ "${run#*=}" = success ] || { echo "coverage: lane $lane run $run" >&2 && status=1; }
        done
        mkdir -p "$out/lanes/$lane"
        cp "$m" "$lanes/$lane/summary.txt" "$lanes/$lane/totals.txt" "$lanes/$lane/uncovered.txt" "$out/lanes/$lane/"
        printf '%-28s %s  %s\n' "$lane" "$(grep '^src ' "$lanes/$lane/totals.txt")" "$(meta runs "$m")" >> "$out/lanes.txt"
    done

    awk -f vendor/pg-clickhouse-c/clickhouse-c/tools/lcov_merge.awk "$lanes"/*/lcov.info > "$out/merged.lcov"
    report "$out/merged.lcov" "$out" "$(meta sources "$first")" || status=1

    # genhtml needs readable sources, which only src/ has here
    awk '/^SF:/ { keep = substr($0, 4) ~ /^src\// } keep' "$out/merged.lcov" > "$out/html.lcov"
    genhtml --quiet --legend --title "pg_clickhouse $(git rev-parse --short HEAD)" \
        --output-directory "$out/html" "$out/html.lcov" || status=1
    rm "$out/html.lcov"
    return $status
}

[ $# -gt 0 ] || die "usage: see $0"
cmd=$1
shift
case $cmd in
    prepare | stop | collect | report | merge) "$cmd" "$@" ;;
    *) die "unknown command $cmd" ;;
esac
