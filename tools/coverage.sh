#!/bin/sh
# Turn gcov counters left beside test/pgch_test.o into an lcov tracefile for
# pg-clickhouse*.h, then require every line covered. Counters accumulate across
# installcheck runs, so report after the last one
set -eu
export LC_ALL=C

cd "$(dirname "$0")/.."
objdir=${1:-$PWD/test}
GCOV=${GCOV:-gcov}
CH_C_DIR=${CH_C_DIR:-clickhouse-c}
COVERAGE_MIN_LINE=${COVERAGE_MIN_LINE:-100}

for notes in "$objdir"/*.gcno; do
    test -f "$notes" || {
        echo "no coverage notes in $objdir, run make -C test coverage-build" >&2
        exit 1
    }
    break
done
for data in "$objdir"/*.gcda; do
    test -f "$data" || {
        echo "no counters in $objdir, run the tests after coverage-build" >&2
        exit 1
    }
    break
done

rm -rf coverage
mkdir coverage

for notes in "$objdir"/*.gcno; do
    "$GCOV" --stdout --source-prefix "$objdir" "$notes"
done > coverage/coverage.gcov

# Keep the library headers, drop PostgreSQL, clickhouse-c, and the test glue
# Drop lines no caller can reach: pg_unreachable() legs, which only keep the
# switch total, together with case labels directly above them
awk -F: '
    function text(line, rest) {
        rest = substr(line, index(line, ":") + 1)
        return substr(rest, index(rest, ":") + 1)
    }
    function flush(i) {
        for (i = 1; i <= npend; i++) {
            print pend[i]
        }
        npend = 0
    }
    $2 == 0 && $3 == "Source" {
        flush()
        file = substr($0, index($0, ":Source:") + 8)
        sub(/^.*\//, "", file)
        include = file ~ /^pg-clickhouse(-[a-z]+)?[.]h$/
        if (include) print "SF:" file
        next
    }
    !include || $2 <= 0 {
        next
    }
    {
        src = text($0)
    }
    $1 ~ /^[ \t]*-[ \t]*$/ {
        next
    }
    src ~ /pg_unreachable\(\)/ {
        npend = 0
        next
    }
    src ~ /^[ \t]*(case[ \t].*:|default:)[ \t]*$/ {
        pend[++npend] = sprintf("DA:%d,%.0f", $2, $1 + 0)
        next
    }
    {
        if ($1 + 0 < 0) {
            print "Negative gcov count: " file ":" $2 > "/dev/stderr"
            exit 1
        }
        flush()
        printf "DA:%d,%.0f\n", $2, $1 + 0
    }
    END { flush() }
' coverage/coverage.gcov > coverage/coverage.lcov

awk -f "$CH_C_DIR/tools/lcov_merge.awk" coverage/coverage.lcov > coverage/lcov.info

: > coverage/missing-lines.txt
status=0
awk -v missing=coverage/missing-lines.txt -v min="$COVERAGE_MIN_LINE" \
    -f tools/lcov_summary.awk coverage/lcov.info > coverage/coverage.txt || status=$?
cat coverage/coverage.txt
exit "$status"
