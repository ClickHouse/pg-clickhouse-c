/* Generator for pgch__is_reserved keyword trie */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Words ClickHouse parses as literal or keyword where identifier expected */
static const char* const words[] = {
    "all",   "case",  "columns",    "constraint", "cube",   "distinct", "else",
    "end",   "false", "from",       "index",      "inf",    "infinity", "nan",
    "not",   "null",  "projection", "recursive",  "rollup", "select",   "some",
    "table", "then",  "top",        "true",       "values", "when",
};

/* String literals wrapped within column limit, adheres to clang-format */
static void
emit_literal(const char* name, const uint8_t* bytes, int len, bool octal) {
    int per_line = (88 - 11) / (octal ? 4 : 1);
    printf("    static const uint8_t %s[] =", name);
    for (int i = 0; i < len; i++) {
        if (i % per_line == 0) {
            printf("%s\n        \"", i ? "\"" : "");
        }
        printf(octal ? "\\%03o" : "%c", bytes[i]);
    }
    printf("\";\n");
}

int
main(void) {
    static uint8_t child[256][26];
    static bool word_end[256];
    int n = 1;

    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) {
        if (!words[i][1]) {
            fprintf(stderr, "gen_reserved_trie: %s shorter than 2 letters\n", words[i]);
            return 1;
        }
        int state = 0;
        for (const char* p = words[i]; *p; p++) {
            int c = *p - 'a';
            if (c < 0 || c >= 26) {
                fprintf(stderr, "gen_reserved_trie: %s not lowercase a-z\n", words[i]);
                return 1;
            }
            if (!child[state][c]) {
                if (n == 256) {
                    fprintf(stderr, "gen_reserved_trie: exceeds uint8_t states\n");
                    return 1;
                }
                child[state][c] = n++;
            }
            state = child[state][c];
        }
        word_end[state] = true;
    }

    /* First letter c enters state c, then breadth first edge i enters state i + 26 */
    int edges          = 0;
    uint8_t order[282] = { 0 }, first[282], labels[255];
    for (int c = 0; c < 26; c++) {
        order[c] = child[0][c];
    }
    for (int state = 0; state < 26 + edges; state++) {
        first[state] = edges;
        for (int c = 0; order[state] && c < 26; c++) {
            int next = child[order[state]][c];
            if (next) {
                order[26 + edges] = next;
                labels[edges++]   = (word_end[next] ? 'A' : 'a') + c;
            }
        }
    }
    first[26 + edges] = edges;

    emit_literal("first", first, 27 + edges, true);
    emit_literal("labels", labels, edges, false);
    return 0;
}
