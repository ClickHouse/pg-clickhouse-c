#!/bin/sh
# Regenerate keyword trie inside pgch__is_reserved
set -eu
cd "$(dirname "$0")/.."

gen=$(mktemp)
trap 'rm -f "$gen" "$gen.out" pg-clickhouse.h.new' EXIT
cc -std=c11 -O2 -Wall -Wextra tools/gen_reserved_trie.c -o "$gen"
"$gen" > "$gen.out"

awk -v block="$gen.out" '
    /END RESERVED TRIE/ { while ((getline line < block) > 0) print line; skip = 0 }
    !skip
    /BEGIN RESERVED TRIE/ { skip = 1 }
' pg-clickhouse.h > pg-clickhouse.h.new
mv pg-clickhouse.h.new pg-clickhouse.h
