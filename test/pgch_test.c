/*
 * pgch_test.c -- SQL surface for exercising pg-clickhouse-c in a backend.
 *
 * Encodes PG Datums into a one-column Native block, hands the bytes back as
 * bytea, and decodes such bytes into text. Round trips run entirely in memory:
 * no ClickHouse server, no chDB. This is also the TU carrying both
 * implementations, so it doubles as the compile check for the headers.
 */

#include "postgres.h"

#include <string.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

PG_MODULE_MAGIC;

void
_PG_init(void);

void
_PG_init(void) {
    pgch_msg_prefix = "pgch: ";
}

/* chdb and clickhouse-local emit neither BlockInfo nor the custom-serialization flag.
 */
static const chc_block_opts block_opts = {};

static chc_type*
parse_ch_type(text* name) {
    char* s = text_to_cstring(name);
    chc_type* t;
    chc_err err = {};

    if (chc_type_parse(s, strlen(s), &pgch_alloc, &t, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "type parse: ");
    }
    return t;
}

static bytea*
encode_rows(text* chtype, Datum* vals, bool* nulls, int nrows, Oid valtype) {
    chc_type* t    = parse_ch_type(chtype);
    pgch_col col   = { .name = "c", .name_len = 1, .type = t };
    pgch_writer* w = pgch_writer_new(CurrentMemoryContext, &col, 1);
    const chc_block_builder* bb;
    pgch_buf buf = {};
    chc_io io;
    chc_err err = {};
    bytea* out;

    for (int i = 0; i < nrows; i++) {
        pgch_append_datum(w, 0, vals[i], valtype, nulls[i]);
    }

    if (pgch_writer_rows(w) != (size_t)nrows) {
        elog(
            ERROR, "writer buffered %zu rows, expected %d", pgch_writer_rows(w), nrows
        );
    }

    bb = pgch_writer_build(w);
    pgch_buf_io(&buf, &io);
    if (chc_block_write(&io, bb, &block_opts, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
    }

    out = (bytea*)palloc(VARHDRSZ + buf.len);
    SET_VARSIZE(out, VARHDRSZ + buf.len);
    memcpy(VARDATA(out), buf.data, buf.len);

    pgch_writer_reset(w);
    pgch_writer_free(w);
    return out;
}

PG_FUNCTION_INFO_V1(pgch_encode);

/* (ch_type text, val anyelement) -> bytea: one-row block. */
Datum
pgch_encode(PG_FUNCTION_ARGS) {
    Datum val   = PG_ARGISNULL(1) ? (Datum)0 : PG_GETARG_DATUM(1);
    bool isnull = PG_ARGISNULL(1);
    Oid valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    PG_RETURN_BYTEA_P(encode_rows(PG_GETARG_TEXT_PP(0), &val, &isnull, 1, valtype));
}

PG_FUNCTION_INFO_V1(pgch_encode_rows);

/* (ch_type text, vals anyarray) -> bytea: one row per array element. */
Datum
pgch_encode_rows(PG_FUNCTION_ARGS) {
    Oid arrtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
    Oid elemtype;
    int16 typlen;
    bool typbyval;
    char typalign;
    Datum* vals;
    bool* nulls;
    int nvals;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        PG_RETURN_NULL();
    }

    elemtype = get_element_type(arrtype);
    if (!OidIsValid(elemtype)) {
        elog(ERROR, "second argument is not an array");
    }
    get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);
    deconstruct_array(
        PG_GETARG_ARRAYTYPE_P(1),
        elemtype,
        typlen,
        typbyval,
        typalign,
        &vals,
        &nulls,
        &nvals
    );

    PG_RETURN_BYTEA_P(encode_rows(PG_GETARG_TEXT_PP(0), vals, nulls, nvals, elemtype));
}

/*
 * Block source over submitted bytes. An ioless chc_in cannot tell a clean end
 * of stream from a truncated block, so CHC_WOULD_BLOCK once every byte is
 * consumed means "no more blocks" here.
 */
typedef struct {
    chc_in* in;
    bool done;
    char* error;
} bytes_source;

static const chc_block*
bytes_next_block(void* ud) {
    bytes_source* s = (bytes_source*)ud;
    chc_block* b    = NULL;
    chc_err err     = {};
    int rc;

    if (s->done) {
        return NULL;
    }
    rc = chc_block_read(s->in, &pgch_alloc, &block_opts, &b, &err);
    if (rc != CHC_OK) {
        s->done = true;
        if (rc != CHC_WOULD_BLOCK || chc_in_available(s->in) > 0) {
            s->error = pstrdup(err.msg[0] ? err.msg : "truncated block");
        }
        return NULL;
    }
    if (!b) {
        s->done = true;
    }
    return b;
}

static const char*
bytes_source_error(void* ud) {
    return ((bytes_source*)ud)->error;
}

PG_FUNCTION_INFO_V1(pgch_decode);

/* (data bytea) -> text[]: column 0 of every row, rendered by its PG type. */
Datum
pgch_decode(PG_FUNCTION_ARGS) {
    bytea* data = PG_GETARG_BYTEA_PP(0);
    bytes_source src;
    pgch_block_source bsrc;
    pgch_reader r;
    chc_err err = {};
    Datum* out  = NULL;
    bool* nulls = NULL;
    int n       = 0;
    int cap     = 0;
    ArrayType* result;

    src.in    = pgch_in_alloc();
    src.done  = false;
    src.error = NULL;
    if (chc_in_init_ioless(src.in, &pgch_alloc) != CHC_OK) {
        elog(ERROR, "chc_in_init_ioless failed");
    }
    if (chc_in_submit(src.in, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data), &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "submit: ");
    }

    bsrc.ud         = &src;
    bsrc.next_block = bytes_next_block;
    bsrc.error      = bytes_source_error;

    pgch_reader_init(&r, &bsrc);
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    if (pgch_reader_columns(&r) != 1) {
        elog(ERROR, "expected 1 column, got %zu", pgch_reader_columns(&r));
    }

    while (pgch_reader_next(&r)) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            out =
                out ? repalloc(out, cap * sizeof(Datum)) : palloc(cap * sizeof(Datum));
            nulls = nulls ? repalloc(nulls, cap * sizeof(bool))
                          : palloc(cap * sizeof(bool));
        }
        nulls[n] = r.nulls[0];
        out[n]   = nulls[n] ? (Datum)0
                            : CStringGetTextDatum(
                                  pgch_value_to_cstring(r.coltypes[0], r.values[0])
                              );
        n++;
    }
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    pgch_reader_free(&r);

    result = construct_md_array(
        out, nulls, 1, &n, (int[]){ 1 }, TEXTOID, -1, false, TYPALIGN_INT
    );
    PG_RETURN_ARRAYTYPE_P(result);
}

PG_FUNCTION_INFO_V1(pgch_pgtype);

/* (ch_type text) -> text: the PG type this CH type decodes into. */
Datum
pgch_pgtype(PG_FUNCTION_ARGS) {
    chc_type* t = parse_ch_type(PG_GETARG_TEXT_PP(0));

    PG_RETURN_TEXT_P(cstring_to_text(format_type_be(pgch_native_oid(t))));
}
