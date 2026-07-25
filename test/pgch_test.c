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

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "parser/parse_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

PG_MODULE_MAGIC;

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
    if (chc_block_write(&io, bb, &pgch_block_opts_local, &err) != CHC_OK) {
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
    rc = chc_block_read(s->in, &pgch_alloc, &pgch_block_opts_local, &b, &err);
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

/*
 * Column 0 of every row of an already-initialized reader, as text.
 * from_type builds the conversion state off the column's CH type rather than
 * off the first non-null value.
 */
static ArrayType*
decode_reader(pgch_reader* r_, Oid outtype, bool from_type) {
    pgch_reader r   = *r_;
    Datum* out      = NULL;
    bool* nulls     = NULL;
    int n           = 0;
    int cap         = 0;
    void* convstate = NULL;
    bool converted  = false;
    FmgrInfo outfn  = {};
    ArrayType* result;

    if (OidIsValid(outtype)) {
        Oid outfuncid;
        bool varlena;

        getTypeOutputInfo(outtype, &outfuncid, &varlena);
        fmgr_info(outfuncid, &outfn);
    }

    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    if (pgch_reader_columns(&r) != 1) {
        elog(ERROR, "expected 1 column, got %zu", pgch_reader_columns(&r));
    }
    if (OidIsValid(outtype) && from_type) {
        convstate = pgch_reader_convert_init(&r, 0, outtype);
        converted = true;
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
        if (nulls[n]) {
            out[n] = (Datum)0;
        } else if (!OidIsValid(outtype)) {
            out[n] =
                CStringGetTextDatum(pgch_value_to_cstring(r.coltypes[0], r.values[0]));
        } else {
            /* State comes from the first non-null value, as the FDW does. */
            if (!converted) {
                convstate = pgch_convert_init(r.values[0], r.coltypes[0], outtype);
                converted = true;
            }
            out[n] = CStringGetTextDatum(
                OutputFunctionCall(&outfn, pgch_convert(convstate, r.values[0]))
            );
        }
        n++;
    }
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    if (convstate) {
        pgch_convert_free(convstate);
    }
    pgch_reader_free(&r);

    result = construct_md_array(
        out, nulls, 1, &n, (int[]){ 1 }, TEXTOID, -1, false, TYPALIGN_INT
    );
    return result;
}

/*
 * Column 0 of every row as text. outtype InvalidOid renders the decoder's own
 * Datum; otherwise every value goes through pgch_convert into outtype first,
 * which is the path a composite target takes.
 */
static ArrayType*
decode_column(bytea* data, Oid outtype, bool from_type) {
    bytes_source src;
    pgch_block_source bsrc;
    pgch_reader r;
    chc_err err = {};

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
    return decode_reader(&r, outtype, from_type);
}

/* Hands the same bytes over in fixed-size pieces, for the refill path. */
typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    size_t chunk;
} chunk_feed;

static bool
feed_next_chunk(void* ud, const void** p, size_t* n, char** error) {
    chunk_feed* f = (chunk_feed*)ud;
    size_t take   = Min(f->chunk, f->len - f->pos);

    (void)error;
    *p = f->data + f->pos;
    *n = take; /* zero once drained: end of stream */
    f->pos += take;
    return true;
}

PG_FUNCTION_INFO_V1(pgch_decode_chunks);

/* (data bytea, chunk int) -> text[]: pgch_decode over a pgch_chunk_source. */
Datum
pgch_decode_chunks(PG_FUNCTION_ARGS) {
    bytea* data = PG_GETARG_BYTEA_PP(0);
    chunk_feed feed;
    pgch_chunk_source src;
    pgch_reader r;

    feed.data  = (const uint8_t*)VARDATA_ANY(data);
    feed.len   = VARSIZE_ANY_EXHDR(data);
    feed.pos   = 0;
    feed.chunk = Max(1, PG_GETARG_INT32(1));

    src.ud         = &feed;
    src.next_chunk = feed_next_chunk;
    src.cancelled  = NULL;

    pgch_reader_init_chunks(&r, &src, NULL);
    PG_RETURN_ARRAYTYPE_P(decode_reader(&r, InvalidOid, false));
}

PG_FUNCTION_INFO_V1(pgch_decode);

/* (data bytea) -> text[]: column 0 of every row, rendered by its PG type. */
Datum
pgch_decode(PG_FUNCTION_ARGS) {
    PG_RETURN_ARRAYTYPE_P(decode_column(PG_GETARG_BYTEA_PP(0), InvalidOid, false));
}

PG_FUNCTION_INFO_V1(pgch_decode_as);

/* (data bytea, target anyelement) -> text[]: rows converted into target's type. */
Datum
pgch_decode_as(PG_FUNCTION_ARGS) {
    Oid outtype = get_fn_expr_argtype(fcinfo->flinfo, 1);

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    if (!OidIsValid(outtype)) {
        elog(ERROR, "could not determine target type");
    }
    PG_RETURN_ARRAYTYPE_P(decode_column(PG_GETARG_BYTEA_PP(0), outtype, false));
}

PG_FUNCTION_INFO_V1(pgch_decode_typed);

/* As pgch_decode_as, with the conversion state built from the column type. */
Datum
pgch_decode_typed(PG_FUNCTION_ARGS) {
    Oid outtype = get_fn_expr_argtype(fcinfo->flinfo, 1);

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    if (!OidIsValid(outtype)) {
        elog(ERROR, "could not determine target type");
    }
    PG_RETURN_ARRAYTYPE_P(decode_column(PG_GETARG_BYTEA_PP(0), outtype, true));
}

PG_FUNCTION_INFO_V1(pgch_pgtype);

/* (ch_type text) -> text: the PG type this CH type decodes into. */
Datum
pgch_pgtype(PG_FUNCTION_ARGS) {
    chc_type* t = parse_ch_type(PG_GETARG_TEXT_PP(0));

    PG_RETURN_TEXT_P(cstring_to_text(format_type_be(pgch_native_oid(t))));
}

PG_FUNCTION_INFO_V1(pgch_native_settings);

/* () -> text: the settings string a query must carry, pinned by expected out. */
Datum
pgch_native_settings(PG_FUNCTION_ARGS pg_attribute_unused()) {
    PG_RETURN_TEXT_P(cstring_to_text(PGCH_NATIVE_SETTINGS));
}

/* json_as_json / low_cardinality / numeric_as_string, from arg `first` on. */
static pgch_type_opts
type_opts(FunctionCallInfo fcinfo, int first) {
    pgch_type_opts opts = {};

    opts.json_as_json      = PG_GETARG_BOOL(first);
    opts.low_cardinality   = PG_GETARG_BOOL(first + 1);
    opts.numeric_as_string = PG_GETARG_BOOL(first + 2);
    return opts;
}

PG_FUNCTION_INFO_V1(pgch_chtype);

/* (decl text, notnull bool, opts...) -> text: CH type for a PG declaration. */
Datum
pgch_chtype(PG_FUNCTION_ARGS) {
    char* decl          = text_to_cstring(PG_GETARG_TEXT_PP(0));
    pgch_type_opts opts = type_opts(fcinfo, 2);
    Oid typid;
    int32 typmod;

#if PG_VERSION_NUM >= 160000
    (void)parseTypeString(decl, &typid, &typmod, NULL);
#else
    parseTypeString(decl, &typid, &typmod, false);
#endif

    PG_RETURN_TEXT_P(cstring_to_text(
        pgch_ch_type_for(typid, typmod, PG_GETARG_BOOL(1), &opts)
    ));
}

PG_FUNCTION_INFO_V1(pgch_structure);

/* (rel regclass, opts...) -> text: the structure clause for a relation. */
Datum
pgch_structure(PG_FUNCTION_ARGS) {
    Relation rel        = table_open(PG_GETARG_OID(0), AccessShareLock);
    pgch_type_opts opts = type_opts(fcinfo, 1);
    char* s             = pgch_structure_from_tupdesc(RelationGetDescr(rel), &opts);

    table_close(rel, AccessShareLock);
    PG_RETURN_TEXT_P(cstring_to_text(s));
}

/* Writer columns from a descriptor, the types pgch_ch_type_for declares. */
static pgch_writer*
writer_for_tupdesc(TupleDesc desc, const pgch_type_opts* opts, size_t* out_ncols) {
    pgch_col* cols = palloc0(desc->natts * sizeof(pgch_col));
    size_t n       = 0;
    pgch_writer* w;

    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);
        char* chtype;
        chc_err err = {};
        chc_type* t;

        if (a->attisdropped || a->attgenerated) {
            continue;
        }
        chtype = pgch_ch_type_for(a->atttypid, a->atttypmod, a->attnotnull, opts);
        if (chc_type_parse(chtype, strlen(chtype), &pgch_alloc, &t, &err) != CHC_OK) {
            pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, "type parse: ");
        }
        cols[n].name     = NameStr(a->attname);
        cols[n].name_len = strlen(NameStr(a->attname));
        cols[n].type     = t;
        n++;
    }

    w          = pgch_writer_new(CurrentMemoryContext, cols, n);
    *out_ncols = n;
    return w;
}

/* table_beginscan gained a caller flags argument in PG 19. */
static TableScanDesc
begin_scan(Relation rel) {
#if PG_VERSION_NUM >= 190000
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
#else
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
#endif
}

PG_FUNCTION_INFO_V1(pgch_table_roundtrip);

/*
 * (rel regclass, opts..., null_array_empty, nonfinite) -> text[]: every row of
 * the relation through the declared structure and back into the column's own
 * PG type, one text per row with columns joined by '|'. The whole COPY shape:
 * pgch_ch_type_for, pgch_append_slot, pgch_writer_flush,
 * pgch_reader_convert_init.
 */
Datum
pgch_table_roundtrip(PG_FUNCTION_ARGS) {
    Relation rel        = table_open(PG_GETARG_OID(0), AccessShareLock);
    pgch_type_opts opts = type_opts(fcinfo, 1);
    TupleDesc desc      = RelationGetDescr(rel);
    size_t ncols;
    pgch_writer* w = writer_for_tupdesc(desc, &opts, &ncols);
    pgch_buf buf   = {};
    TableScanDesc scan;
    TupleTableSlot* slot;
    bytea* bytes;
    Oid* targets  = palloc0(ncols * sizeof(Oid));
    FmgrInfo* out = palloc0(ncols * sizeof(FmgrInfo));
    void** states;
    bytes_source src;
    pgch_block_source bsrc;
    pgch_reader r;
    chc_err err = {};
    Datum* rows = NULL;
    int nrows   = 0;
    int cap     = 0;
    size_t j    = 0;

    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);
        Oid outfunc;
        bool varlena;

        if (a->attisdropped || a->attgenerated) {
            continue;
        }
        targets[j] = a->atttypid;
        getTypeOutputInfo(a->atttypid, &outfunc, &varlena);
        fmgr_info(outfunc, &out[j]);
        j++;
    }

    if (PG_GETARG_BOOL(4)) {
        pgch_writer_set_null_array(w, PGCH_NULL_ARRAY_EMPTY);
    }
    pgch_writer_set_nonfinite(w, (pgch_nonfinite)PG_GETARG_INT32(5));

    scan = begin_scan(rel);
    slot = table_slot_create(rel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        pgch_append_slot(w, slot);
    }
    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    table_close(rel, AccessShareLock);

    pgch_writer_flush(w, &buf, NULL);
    bytes = (bytea*)palloc(VARHDRSZ + buf.len);
    SET_VARSIZE(bytes, VARHDRSZ + buf.len);
    memcpy(VARDATA(bytes), buf.data, buf.len);
    pgch_writer_free(w);

    src.in    = pgch_in_alloc();
    src.done  = false;
    src.error = NULL;
    if (chc_in_init_ioless(src.in, &pgch_alloc) != CHC_OK) {
        elog(ERROR, "chc_in_init_ioless failed");
    }
    if (chc_in_submit(src.in, VARDATA(bytes), VARSIZE_ANY_EXHDR(bytes), &err) !=
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

    /* State off the column type, so an all-NULL column needs no first row. */
    states = palloc0(ncols * sizeof(void*));
    for (size_t i = 0; i < ncols; i++) {
        states[i] = pgch_reader_convert_init(&r, i, targets[i]);
    }

    while (pgch_reader_next(&r)) {
        StringInfoData row;
        Datum* values = palloc0(ncols * sizeof(Datum));
        bool* nulls   = palloc0(ncols * sizeof(bool));

        pgch_reader_fill(&r, states, values, nulls);
        initStringInfo(&row);
        for (size_t i = 0; i < ncols; i++) {
            if (i) {
                appendStringInfoChar(&row, '|');
            }
            appendStringInfoString(
                &row, nulls[i] ? "NULL" : OutputFunctionCall(&out[i], values[i])
            );
        }

        if (nrows == cap) {
            cap  = cap ? cap * 2 : 8;
            rows = rows ? repalloc(rows, cap * sizeof(Datum))
                        : palloc(cap * sizeof(Datum));
        }
        rows[nrows++] = CStringGetTextDatum(row.data);
    }
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    pgch_reader_free(&r);

    PG_RETURN_ARRAYTYPE_P(construct_md_array(
        rows, NULL, 1, &nrows, (int[]){ 1 }, TEXTOID, -1, false, TYPALIGN_INT
    ));
}
