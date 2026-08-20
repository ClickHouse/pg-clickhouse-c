#!/usr/bin/awk -f
# Table comes from the expected output of the type_table regression test
#
# usage: gen_type_table.awk [markdown-file]

function die(msg) {
    print "gen_type_table: " msg > "/dev/stderr"
    exit 1
}

BEGIN {
    target = ARGV[1]
    ARGV[1] = "test/expected/type_table.out"
    ARGC = 2
    if (target != "") ARGV[ARGC++] = target
}

# psql echoes the query between the markers, so take the table rows alone
# Its border 2 output is markdown once the header rule turns into pipes, the
# top and bottom rules dropping with the rest of the non-table lines
FNR == NR {
    if (/^TYPE-TABLE-END$/) on = 0
    if (on && /^\+/ && ++rule == 2) gsub(/\+/, "|")
    if (on && /^\|/) table = table $0 "\n"
    if (/^TYPE-TABLE-BEGIN$/) on = 1
    next
}

{
    lines[n = FNR] = $0
    if (/TYPE-TABLE-BEGIN/) begin = 1
    if (/TYPE-TABLE-END/) end = 1
}

END {
    sub(/\n$/, "", table)
    if (table == "") die("no table between the markers of " ARGV[1])
    if (target == "") {
        print table
        exit
    }
    if (!begin || !end) die(target " marks no table to replace")

    # target is buffered, so rewriting it in place needs no temporary file
    for (i = 1; i <= n; i++) {
        if (lines[i] ~ /TYPE-TABLE-BEGIN/) {
            print lines[i] > (target)
            print table > (target)
            skip = 1
            continue
        }
        if (lines[i] ~ /TYPE-TABLE-END/) skip = 0
        if (!skip) print lines[i] > (target)
    }
    close(target)
    print "gen_type_table: " target " updated" > "/dev/stderr"
}
