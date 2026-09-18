#!/usr/bin/awk -f
# Per-file line coverage table from a merged lcov tracefile
# Write uncovered lines to the file named by missing, exit 2 below min percent
BEGIN {
    FS = ":"
    if (min == "") {
        min = 100
    }
    printf "%-28s %8s %8s %9s\n", "File", "Covered", "Lines", "Coverage"
}
/^SF:/ { file = substr($0, 4) }
/^DA:/ {
    split($2, data, ",")
    if (data[2] + 0 == 0) print file ":" data[1] > missing
}
/^LF:/ { lines = $2; total += lines }
/^LH:/ {
    hits = $2; covered += hits
    printf "%-28s %8d %8d %8.2f%%\n", file, hits, lines, lines ? 100 * hits / lines : 0
}
END {
    printf "%-28s %8d %8d %8.2f%%\n", "TOTAL", covered, total, total ? 100 * covered / total : 0
    if (!total || 100 * covered / total < min) {
        printf "Require %s%% line coverage, see %s\n", min, missing
        exit 2
    }
}
