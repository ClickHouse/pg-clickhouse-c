#!/usr/bin/awk -f
# Table comes from the expected output of the type_table regression test
#
# usage: gen_type_table.awk [markdown-file]

function die(msg) {
    print "gen_type_table: " msg > "/dev/stderr"
    exit (failed = 1)
}

BEGIN {
    src = "test/expected/type_table.out"

    # psql echoes table between markers, convert to markdown table
    while ((getline line < src) > 0) {
        if (line ~ /^TYPE-TABLE-END$/) break
        if (!inside) {
            inside = line ~ /^TYPE-TABLE-BEGIN$/
            continue
        }
        if (line ~ /^\+/ && ++rules == 2) gsub(/\+/, "|", line)
        if (line ~ /^\|/) table = table line "\n"
    }
    close(src)
    if (table == "") die("no table between the markers of " src)
    sub(/\n$/, "", table)

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
    print "gen_type_table: " target " updated" > "/dev/stderr"
}
