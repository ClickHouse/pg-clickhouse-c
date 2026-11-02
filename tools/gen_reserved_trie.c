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

    /* Renumber breadth first so edge i enters state i + 1 */
    int edges            = 0;
    uint8_t order[256]   = { 0 }, first[257], labels[255];
    uint64_t terminal[4] = { 0 };
    for (int state = 0; state < n; state++) {
        first[state] = edges;
        for (int c = 0; c < 26; c++) {
            int next = child[order[state]][c];
            if (next) {
                labels[edges++] = 'a' + c;
                order[edges]    = next;
                if (word_end[next]) {
                    terminal[edges >> 6] |= UINT64_C(1) << (edges & 63);
                }
            }
        }
    }
    first[n] = edges;

    emit_literal("first", first, n + 1, true);
    emit_literal("labels", labels, edges, false);
    printf("    static const uint64_t terminal[] = {\n");
    for (int w = 0; w <= (n - 1) / 64; w++) {
        printf("        UINT64_C(0x%016llx),\n", (unsigned long long)terminal[w]);
    }
    printf("    };\n");
    return 0;
}
