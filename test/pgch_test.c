/*
 * Expose pg-clickhouse-c through SQL tests
 *
 * Run Native round trips in memory without ClickHouse or chDB
 * Compile both header implementations here
 */

#include "postgres.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"

#define CHC_IMPLEMENTATION
#define PGCH_IMPLEMENTATION
#include "clickhouse.h"

/* Scope allocation faults to library calls, preserve PostgreSQL ERROR semantics */
static int alloc_fail_at;
static int alloc_seen;
static bool alloc_failed;

static void
alloc_fault(void) {
    if (alloc_fail_at && ++alloc_seen == alloc_fail_at) {
        alloc_fail_at = 0;
        alloc_failed  = true;
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("injected OOM")));
    }
}

static void*
fault_alloc_huge(MemoryContext cxt, Size size) {
    alloc_fault();
    return MemoryContextAllocHuge(cxt, size);
}

static void*
fault_realloc_huge(void* ptr, Size size) {
    alloc_fault();
    return repalloc_huge(ptr, size);
}

#define MemoryContextAllocHuge fault_alloc_huge
#define repalloc_huge fault_realloc_huge

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#undef MemoryContextAllocHuge
#undef repalloc_huge

PG_MODULE_MAGIC;

static chc_type*
parse_ch_type_cstr(const char* s, const char* where) {
    chc_type* t;
    chc_err err = {};

    if (chc_type_parse(s, strlen(s), &pgch_alloc, &t, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_INVALID_PARAMETER_VALUE, NULL, where);
    }
    return t;
}

static chc_type*
parse_ch_type(text* name, const char* where) {
    return parse_ch_type_cstr(text_to_cstring(name), where);
}

static bytea*
bytea_from_buf(const pgch_buf* buf) {
    bytea* out = (bytea*)palloc(VARHDRSZ + buf->len);

    SET_VARSIZE(out, VARHDRSZ + buf->len);
    memcpy(VARDATA(out), buf->data, buf->len);
    return out;
}

static bytea*
encode_rows(text* chtype, Datum* vals, bool* nulls, int nrows, Oid valtype) {
    chc_type* t    = parse_ch_type(chtype, "column c");
    pgch_col col   = { .name = "c", .name_len = 1, .type = t };
    pgch_writer* w = pgch_writer_new(CurrentMemoryContext, &col, 1);
    pgch_buf buf   = {};
    chc_io io;
    chc_err err = {};

    for (int i = 0; i < nrows; i++) {
        pgch_append_datum(w, 0, vals[i], valtype, nulls[i]);
    }

    if (pgch_writer_rows(w) != (size_t)nrows) {
        elog(
            ERROR, "writer buffered %zu rows, expected %d", pgch_writer_rows(w), nrows
        );
    }

    const chc_block_builder* bb = pgch_writer_build(w);
    pgch_buf_io(&buf, &io);
    if (chc_block_write(&io, bb, &pgch_block_opts_local, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
    }

    bytea* out = bytea_from_buf(&buf);

    pgch_writer_reset(w);
    pgch_writer_free(w);
    return out;
}

PG_FUNCTION_INFO_V1(pgch_encode);

/* Encode one row into one-column Native block */
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

/* Encode one row per array element */
Datum
pgch_encode_rows(PG_FUNCTION_ARGS) {
    Oid arrtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
    int16 typlen;
    bool typbyval;
    char typalign;
    Datum* vals;
    bool* nulls;
    int nvals;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1)) {
        PG_RETURN_NULL();
    }

    Oid elemtype = get_element_type(arrtype);
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

PG_FUNCTION_INFO_V1(pgch_encode_valid_rows);

/* Test recovery after value conversion fails */
Datum
pgch_encode_valid_rows(PG_FUNCTION_ARGS) {
    Oid arrtype  = get_fn_expr_argtype(fcinfo->flinfo, 1);
    Oid elemtype = get_element_type(arrtype);
    int16 typlen;
    bool typbyval;
    char typalign;
    Datum* vals;
    bool* nulls;
    int nvals;
    chc_type* t                = parse_ch_type(PG_GETARG_TEXT_PP(0), "column c");
    pgch_col col               = { .name = "c", .name_len = 1, .type = t };
    pgch_writer* w             = pgch_writer_new(CurrentMemoryContext, &col, 1);
    pgch_buf buf               = {};
    pgch_checkpoint checkpoint = {};
    chc_io io;
    chc_err err       = {};
    MemoryContext old = CurrentMemoryContext;

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

    for (int i = 0; i < nvals; i++) {
        pgch_writer_checkpoint(w, &checkpoint);
        PG_TRY();
        { pgch_append_datum(w, 0, vals[i], elemtype, nulls[i]); }
        PG_CATCH();
        {
            MemoryContextSwitchTo(old);
            FlushErrorState();
            pgch_writer_rollback(w, &checkpoint);
        }
        PG_END_TRY();
    }

    pgch_buf_io(&buf, &io);
    if (chc_block_write(&io, pgch_writer_build(w), &pgch_block_opts_local, &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
    }
    pgch_checkpoint_free(&checkpoint);
    pgch_writer_free(w);
    PG_RETURN_BYTEA_P(bytea_from_buf(&buf));
}

PG_FUNCTION_INFO_V1(pgch_encode_pairs);

/*
 * Encode one row of key & value pairs through the nesting cursor
 * Fill fields per pair, so counts other than two exercise Tuple arity
 * Wrap each value in its own Tuple when nest is set
 */
Datum
pgch_encode_pairs(PG_FUNCTION_ARGS) {
    chc_type* t    = parse_ch_type(PG_GETARG_TEXT_PP(0), "column c");
    pgch_col col   = { .name = "c", .name_len = 1, .type = t };
    pgch_writer* w = pgch_writer_new(CurrentMemoryContext, &col, 1);
    int fields     = PG_GETARG_INT32(3);
    bool nest      = PG_GETARG_BOOL(4);
    Datum* keys;
    bool* keynulls;
    int nkeys;
    Datum* vals;
    bool* valnulls;
    int nvals;
    pgch_buf buf = {};
    chc_io io;
    chc_err err = {};

    deconstruct_array(
        PG_GETARG_ARRAYTYPE_P(1),
        TEXTOID,
        -1,
        false,
        TYPALIGN_INT,
        &keys,
        &keynulls,
        &nkeys
    );
    deconstruct_array(
        PG_GETARG_ARRAYTYPE_P(2),
        INT8OID,
        8,
        FLOAT8PASSBYVAL,
        TYPALIGN_DOUBLE,
        &vals,
        &valnulls,
        &nvals
    );
    if (nkeys != nvals) {
        elog(ERROR, "key and value counts differ");
    }

    pgch_array_begin(w, 0);
    for (int i = 0; i < nkeys; i++) {
        pgch_tuple_begin(w, 0);
        for (int f = 0; f < fields; f++) {
            if (f % 2 == 0) {
                pgch_append_datum(w, 0, keys[i], TEXTOID, keynulls[i]);
            } else {
                if (nest) {
                    pgch_tuple_begin(w, 0);
                }
                pgch_append_datum(w, 0, vals[i], INT8OID, valnulls[i]);
                if (nest) {
                    pgch_tuple_end(w);
                }
            }
        }
        pgch_tuple_end(w);
    }
    pgch_array_end(w);

    if (pgch_nest_active(w)) {
        elog(ERROR, "nesting left open");
    }
    if (pgch_writer_rows(w) != 1) {
        elog(ERROR, "writer buffered %zu rows, expected 1", pgch_writer_rows(w));
    }

    pgch_buf_io(&buf, &io);
    if (chc_block_write(&io, pgch_writer_build(w), &pgch_block_opts_local, &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
    }
    pgch_writer_free(w);
    PG_RETURN_BYTEA_P(bytea_from_buf(&buf));
}

/* I/O-free clickhouse-c input reports CHC_WOULD_BLOCK after submitted bytes */
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

    if (s->done) {
        return NULL;
    }
    int rc = chc_block_read(s->in, &pgch_alloc, &pgch_block_opts_local, &b, &err);
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

/* Initialize reader over bytea, caller keeps src alive */
static void
reader_from_bytea(pgch_reader* r, bytes_source* src, bytea* data) {
    pgch_block_source bsrc;
    chc_err err = {};

    src->in    = pgch_in_alloc();
    src->done  = false;
    src->error = NULL;
    if (chc_in_init_ioless(src->in, &pgch_alloc) != CHC_OK) {
        elog(ERROR, "chc_in_init_ioless failed");
    }
    if (chc_in_submit(src->in, VARDATA_ANY(data), VARSIZE_ANY_EXHDR(data), &err) !=
        CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "submit: ", NULL);
    }

    bsrc.ud         = src;
    bsrc.next_block = bytes_next_block;
    bsrc.error      = bytes_source_error;

    pgch_reader_init(r, &bsrc);
}

/* Decode first column as text, or into outtype when valid */
static Datum
decode_reader(pgch_reader* r, Oid outtype, int32 outtypmod) {
    ArrayBuildState* out = initArrayResult(TEXTOID, CurrentMemoryContext, false);
    void* convstate      = NULL;
    FmgrInfo outfn       = {};

    if (OidIsValid(outtype)) {
        Oid outfuncid;
        bool typisvarlena;

        getTypeOutputInfo(outtype, &outfuncid, &typisvarlena);
        fmgr_info(outfuncid, &outfn);
    }

    if (r->error) {
        elog(ERROR, "decode: %s", r->error);
    }
    if (pgch_reader_columns(r) != 1) {
        elog(ERROR, "expected 1 column, got %zu", pgch_reader_columns(r));
    }
    if (OidIsValid(outtype)) {
        convstate = pgch_reader_convert_init(r, 0, outtype, outtypmod);
    }

    while (pgch_reader_next(r)) {
        bool isnull = r->nulls[0];
        Datum val   = (Datum)0;

        if (isnull) {
        } else if (!OidIsValid(outtype)) {
            val = CStringGetTextDatum(pgch_value_to_cstring(
                chc_block_column_type(r->cur, 0), r->values[0], r->encoding_check
            ));
        } else {
            val = CStringGetTextDatum(
                OutputFunctionCall(&outfn, pgch_convert(convstate, r->values[0]))
            );
        }
        accumArrayResult(out, val, isnull, TEXTOID, CurrentMemoryContext);
    }
    if (r->error) {
        elog(ERROR, "decode: %s", r->error);
    }
    if (pgch_reader_next(r)) {
        elog(ERROR, "reader returned a row past the end of the stream");
    }
    if (convstate) {
        pgch_convert_free(convstate);
    }
    pgch_reader_free(r);

    return makeArrayResult(out, CurrentMemoryContext);
}

static Datum
decode_column(bytea* data, Oid outtype, int32 outtypmod) {
    bytes_source src;
    pgch_reader r;

    reader_from_bytea(&r, &src, data);
    return decode_reader(&r, outtype, outtypmod);
}

/* Arguments carry no type modifier at run time, so read it off the call site */
static int32
arg_typmod(FunctionCallInfo fcinfo, int argno) {
    Node* expr = fcinfo->flinfo ? fcinfo->flinfo->fn_expr : NULL;

    if (!expr || !IsA(expr, FuncExpr) ||
        argno >= list_length(((FuncExpr*)expr)->args)) {
        return -1;
    }
    return exprTypmod((Node*)list_nth(((FuncExpr*)expr)->args, argno));
}

/* Supply fixed-size chunks, failing or cancelling on a chosen chunk */
typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    size_t chunk;
    int calls;
    int fail_at;
    int cancel_at;
} chunk_feed;

static bool
feed_next_chunk(void* ud, const void** p, size_t* n, char** error) {
    chunk_feed* f = (chunk_feed*)ud;
    size_t take   = Min(f->chunk, f->len - f->pos);

    f->calls++;
    /* Negative fail_at fails without a message, which the reader stands in for */
    if (f->fail_at && f->calls >= abs(f->fail_at)) {
        if (f->fail_at > 0) {
            *error = pstrdup("chunk source gave up");
        }
        return false;
    }
    *p = f->data + f->pos;
    *n = take;
    f->pos += take;
    return true;
}

static bool
feed_cancelled(void* ud) {
    chunk_feed* f = (chunk_feed*)ud;

    return f->calls >= f->cancel_at;
}

PG_FUNCTION_INFO_V1(pgch_decode_chunks);

/* Decode Native bytes through chunk source */
Datum
pgch_decode_chunks(PG_FUNCTION_ARGS) {
    bytea* data = PG_GETARG_BYTEA_PP(0);
    chunk_feed feed;
    pgch_chunk_source src;
    pgch_reader r;

    feed.data      = (const uint8_t*)VARDATA_ANY(data);
    feed.len       = VARSIZE_ANY_EXHDR(data);
    feed.pos       = 0;
    feed.chunk     = Max(1, PG_GETARG_INT32(1));
    feed.calls     = 0;
    feed.fail_at   = PG_GETARG_INT32(2);
    feed.cancel_at = PG_GETARG_INT32(3);

    src.ud         = &feed;
    src.next_chunk = feed_next_chunk;
    src.cancelled  = feed.cancel_at ? feed_cancelled : NULL;

    pgch_reader_init_chunks(&r, &src, NULL);
    PG_RETURN_DATUM(decode_reader(&r, InvalidOid, -1));
}

PG_FUNCTION_INFO_V1(pgch_decode);

/* Decode first column of every row as text */
Datum
pgch_decode(PG_FUNCTION_ARGS) {
    bytes_source src;
    pgch_reader r;

    reader_from_bytea(&r, &src, PG_GETARG_BYTEA_PP(0));
    r.encoding_check = (pgch_encoding_check)PG_GETARG_INT32(1);
    PG_RETURN_DATUM(decode_reader(&r, InvalidOid, -1));
}

PG_FUNCTION_INFO_V1(pgch_decode_as);

/* Decode rows into requested PostgreSQL type */
Datum
pgch_decode_as(PG_FUNCTION_ARGS) {
    Oid outtype = get_fn_expr_argtype(fcinfo->flinfo, 1);

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    if (!OidIsValid(outtype)) {
        elog(ERROR, "could not determine target type");
    }
    PG_RETURN_DATUM(
        decode_column(PG_GETARG_BYTEA_PP(0), outtype, arg_typmod(fcinfo, 1))
    );
}

PG_FUNCTION_INFO_V1(pgch_decode_text);

/* Decode rows into PostgreSQL text */
Datum
pgch_decode_text(PG_FUNCTION_ARGS) {
    bytes_source src;
    pgch_reader r;

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }

    reader_from_bytea(&r, &src, PG_GETARG_BYTEA_PP(0));
    r.encoding_check = (pgch_encoding_check)PG_GETARG_INT32(1);
    PG_RETURN_DATUM(decode_reader(&r, TEXTOID, 0));
}

PG_FUNCTION_INFO_V1(pgch_pgtype);

/* Return PostgreSQL type for ClickHouse declaration */
Datum
pgch_pgtype(PG_FUNCTION_ARGS) {
    chc_type* t = parse_ch_type(PG_GETARG_TEXT_PP(0), NULL);

    PG_RETURN_TEXT_P(cstring_to_text(format_type_be(pgch_native_oid(t))));
}

PG_FUNCTION_INFO_V1(pgch_pgcolumn);

/* Return PostgreSQL column descriptor for ClickHouse declaration */
Datum
pgch_pgcolumn(PG_FUNCTION_ARGS) {
    chc_type* t       = parse_ch_type(PG_GETARG_TEXT_PP(0), NULL);
    pgch_pg_type type = pgch_pg_type_for(t, NULL);
    TupleDesc desc    = NULL;
    Datum values[5]   = {};
    bool nulls[5]     = {};

    if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE) {
        elog(ERROR, "expected composite result type");
    }
    if (OidIsValid(type.typid)) {
        values[0] =
            CStringGetTextDatum(format_type_with_typemod(type.typid, type.typmod));
    } else {
        nulls[0] = true;
    }
    values[1] = Int32GetDatum(type.ndims);
    values[2] = BoolGetDatum(type.nullable);
    values[3] = BoolGetDatum(type.truncated);
    values[4] = BoolGetDatum(pgch_pg_type_is_column(type));
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

/* ---- documented type table ------------------------------------------ */
typedef struct type_doc {
    const char* name;
    const char* params;
    const char* args;
    const char* column;
    const char* note;
    const char* omit; /* Reason a mapped type is omitted */
} type_doc;

static const type_doc type_docs[] = {
    { "AggregateFunction", NULL, "sum, Int64" },
    { "Array", "T", "Int32", "T[]", "One PG array type per depth" },
    { "BFloat16", NULL, NULL, NULL, "Write drops low mantissa bits" },
    { "DateTime64", "P", "3", "timestamp(P) with time zone", "P over 6 caps at 6" },
    { "Decimal", "P,S", "9,4", "numeric(P,S)" },
    { "Decimal32", "S", "4", "numeric(9,S)" },
    { "Decimal64", "S", "4", "numeric(18,S)" },
    { "Decimal128", "S", "4", "numeric(38,S)" },
    { "Decimal256", "S", "4", "numeric(76,S)" },
    { "Enum8", NULL, "'a' = 1" },
    { "Enum16", NULL, "'a' = 1" },
    { "FixedString", "N", "5", NULL, "N counts CH bytes, PG characters" },
    { "IntervalNanosecond", NULL, NULL, NULL, "Truncates to microsecond" },
    { "LowCardinality", "T", "String", "T" },
    { "Map", "K,V", "String, Int64", NULL, "One record per pair" },
    { "Nested", "...", "a Int32", NULL, "One record per nested row" },
    { "Nullable", "T", "Int32", "T", "Sets nullable on the column" },
    { "Object",
     NULL, "'json'",
     NULL, NULL,
     "serializes as a materialized Tuple, unlike JSON" },
    { "QBit", NULL, "BFloat16, 16" },
    { "SimpleAggregateFunction", "f,T", "sum, Int64", "T", "Stores values as T" },
    { "Time64", "P", "3", "time(P) without time zone", "P over 6 caps at 6" },
    { "Tuple", "...", "Int32, String", NULL, "Pseudo type, no column takes it" },
    { "Variant", NULL, "Int32, String" },
};

typedef struct type_scan {
    char** rows;
    int nrows;
    char** omitted;
    int nomitted;
} type_scan;

static const type_doc*
find_type_doc(const char* name) {
    for (unsigned i = 0; i < lengthof(type_docs); i++) {
        if (strcmp(type_docs[i].name, name) == 0) {
            return &type_docs[i];
        }
    }
    return NULL;
}

static char*
type_decl(const char* name, const char* args) {
    return args ? psprintf("%s(%s)", name, args) : pstrdup(name);
}

/* Format mapped column, including pseudo types */
static char*
column_cell(const chc_type* t) {
    pgch_pg_type type = pgch_pg_type_for(t, NULL);
    StringInfoData out;

    if (!OidIsValid(type.typid)) {
        return NULL;
    }
    initStringInfo(&out);
    appendStringInfoString(&out, format_type_with_typemod(type.typid, type.typmod));
    for (int i = 1; i < type.ndims; i++) {
        appendStringInfoString(&out, "[]");
    }
    return out.data;
}

/* Turn parse and mapping errors into omission reasons */
static char*
probe_column(const char* decl, const char** reason) {
    MemoryContext outer = CurrentMemoryContext;
    ResourceOwner owner = CurrentResourceOwner;
    char* volatile cell;

    *reason = "pgch: no PostgreSQL type";
    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(outer);
    PG_TRY();
    {
        cell = column_cell(parse_ch_type_cstr(decl, NULL));
        ReleaseCurrentSubTransaction();
    }
    PG_CATCH();
    {
        cell = NULL;
        MemoryContextSwitchTo(outer);

        ErrorData* edata = CopyErrorData();
        *reason          = pstrdup(edata->message);
        FreeErrorData(edata);
        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
    }
    PG_END_TRY();
    MemoryContextSwitchTo(outer);
    CurrentResourceOwner = owner;
    return cell;
}

/* Probe type or record why it is omitted */
static void
scan_type(type_scan* s, const char* name) {
    const type_doc* doc = find_type_doc(name);
    const char* params  = doc ? doc->params : NULL;
    char* decl          = type_decl(name, doc ? doc->args : NULL);
    const char* reason;
    const char* cell = probe_column(decl, &reason);

    if (!cell) {
        s->omitted[s->nomitted++] = psprintf("%s\t%s", decl, reason);
        return;
    }
    if (doc && doc->omit) {
        s->omitted[s->nomitted++] = psprintf("%s\t%s", decl, doc->omit);
        return;
    }
    if (doc && doc->column) {
        cell = doc->column;
    }
    s->rows[s->nrows++] = psprintf(
        "%s\t%s\t%s",
        params ? psprintf("%s(%s)", name, params) : name,
        cell,
        doc && doc->note ? doc->note : ""
    );
}

static size_t
letters(const char* s) {
    size_t n = 0;

    while (isalpha((unsigned char)s[n])) {
        n++;
    }
    return n;
}

/* Order lines by leading name, reading a numeric suffix as a number */
static int
cmp_type_row(const void* a, const void* b) {
    const char* x = *(const char* const*)a;
    const char* y = *(const char* const*)b;
    size_t nx     = letters(x);
    size_t ny     = letters(y);
    int cmp       = strncmp(x, y, Min(nx, ny));

    if (cmp) {
        return cmp;
    }
    if (nx != ny) {
        return nx < ny ? -1 : 1;
    }
    return atoi(x + nx) - atoi(y + ny);
}

/* Probe every type name the parser resolves */
static type_scan
scan_types(void) {
    int cap     = lengthof(chc__name_rows) + lengthof(type_docs);
    type_scan s = {
        .rows    = palloc0(cap * sizeof(char*)),
        .omitted = palloc0(cap * sizeof(char*)),
    };

    for (unsigned i = 0; i < lengthof(chc__name_rows); i++) {
        const struct chc__name_row* row = &chc__name_rows[i];

        scan_type(&s, pnstrdup(chc__name_blob + row->off, row->len));
    }
    for (unsigned i = 0; i < lengthof(type_docs); i++) {
        const char* name = type_docs[i].name;

        if (!chc__name_lookup(name, strlen(name))) {
            scan_type(&s, name);
        }
    }
    qsort(s.rows, s.nrows, sizeof(char*), cmp_type_row);
    qsort(s.omitted, s.nomitted, sizeof(char*), cmp_type_row);
    return s;
}

static void
add_line(ArrayBuildState* out, const char* line) {
    accumArrayResult(
        out, CStringGetTextDatum(line), false, TEXTOID, CurrentMemoryContext
    );
}

static Datum
lines_array(char** lines, int n) {
    ArrayBuildState* out = initArrayResult(TEXTOID, CurrentMemoryContext, false);

    for (int i = 0; i < n; i++) {
        add_line(out, lines[i]);
    }
    return makeArrayResult(out, CurrentMemoryContext);
}

PG_FUNCTION_INFO_V1(pgch_type_table);

/* Return the type table rows, tab separated for psql to lay out */
Datum
pgch_type_table(PG_FUNCTION_ARGS pg_attribute_unused()) {
    type_scan s = scan_types();

    PG_RETURN_DATUM(lines_array(s.rows, s.nrows));
}

PG_FUNCTION_INFO_V1(pgch_type_omitted);

/* Return the declarations the type table leaves out, tab separated */
Datum
pgch_type_omitted(PG_FUNCTION_ARGS pg_attribute_unused()) {
    type_scan s = scan_types();

    PG_RETURN_DATUM(lines_array(s.omitted, s.nomitted));
}

PG_FUNCTION_INFO_V1(pgch_native_settings);

/* Return required Native query settings */
Datum
pgch_native_settings(PG_FUNCTION_ARGS pg_attribute_unused()) {
    PG_RETURN_TEXT_P(cstring_to_text(PGCH_NATIVE_SETTINGS));
}

static pgch_type_opts
type_opts(FunctionCallInfo fcinfo, int first) {
    pgch_type_opts opts = {};

    opts.json_as_json      = PG_GETARG_BOOL(first);
    opts.low_cardinality   = PG_GETARG_BOOL(first + 1);
    opts.numeric_as_string = PG_GETARG_BOOL(first + 2);
    return opts;
}

PG_FUNCTION_INFO_V1(pgch_chtype);

/* Return ClickHouse type for PostgreSQL declaration */
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

    PG_RETURN_TEXT_P(
        cstring_to_text(pgch_ch_type_for(typid, typmod, PG_GETARG_BOOL(1), &opts))
    );
}

PG_FUNCTION_INFO_V1(pgch_structure);

/* Return ClickHouse structure for relation */
Datum
pgch_structure(PG_FUNCTION_ARGS) {
    Relation rel        = table_open(PG_GETARG_OID(0), AccessShareLock);
    pgch_type_opts opts = type_opts(fcinfo, 1);
    /* Library reads NULL opts as every option unset */
    bool any = opts.json_as_json || opts.low_cardinality || opts.numeric_as_string;
    char* s  = pgch_structure_from_tupdesc(RelationGetDescr(rel), any ? &opts : NULL);

    table_close(rel, AccessShareLock);
    PG_RETURN_TEXT_P(cstring_to_text(s));
}

static pgch_writer*
writer_for_tupdesc(TupleDesc desc, const pgch_type_opts* opts, size_t* out_ncols) {
    pgch_col* cols = palloc0(desc->natts * sizeof(pgch_col));
    size_t n       = 0;

    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);
        chc_err err         = {};
        chc_type* t;

        if (!pgch_attr_is_streamed(a)) {
            continue;
        }
        char* chtype = pgch_ch_type_for(a->atttypid, a->atttypmod, a->attnotnull, opts);
        if (chc_type_parse(chtype, strlen(chtype), &pgch_alloc, &t, &err) != CHC_OK) {
            pgch_raise(
                &err,
                ERRCODE_INVALID_PARAMETER_VALUE,
                NULL,
                psprintf("column \"%s\"", NameStr(a->attname))
            );
        }
        cols[n].name     = NameStr(a->attname);
        cols[n].name_len = strlen(NameStr(a->attname));
        cols[n].type     = t;
        n++;
    }

    pgch_writer* w = pgch_writer_new(CurrentMemoryContext, cols, n);
    *out_ncols     = n;
    return w;
}

/* PostgreSQL 19 added caller flags to table_beginscan */
static TableScanDesc
begin_scan(Relation rel) {
#if PG_VERSION_NUM >= 190000
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL, 0);
#else
    return table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
#endif
}

PG_FUNCTION_INFO_V1(pgch_table_roundtrip);

/* Round-trip every relation row through generated ClickHouse structure */
Datum
pgch_table_roundtrip(PG_FUNCTION_ARGS) {
    Relation rel        = table_open(PG_GETARG_OID(0), AccessShareLock);
    pgch_type_opts opts = type_opts(fcinfo, 1);
    TupleDesc desc      = RelationGetDescr(rel);
    size_t ncols;
    pgch_writer* w = writer_for_tupdesc(desc, &opts, &ncols);
    pgch_buf buf   = {};
    Oid* targets   = palloc0(ncols * sizeof(Oid));
    int32* typmods = palloc0(ncols * sizeof(int32));
    FmgrInfo* out  = palloc0(ncols * sizeof(FmgrInfo));
    /* Fill by attribute, so dropped and generated attributes leave holes */
    int* dest     = palloc0(ncols * sizeof(int));
    Datum* values = palloc0(desc->natts * sizeof(Datum));
    bool* nulls   = palloc0(desc->natts * sizeof(bool));
    bytes_source src;
    pgch_reader r;
    ArrayBuildState* rows = initArrayResult(TEXTOID, CurrentMemoryContext, false);
    size_t j              = 0;

    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);
        Oid outfunc;
        bool typisvarlena;

        if (!pgch_attr_is_streamed(a)) {
            continue;
        }
        targets[j] = a->atttypid;
        typmods[j] = a->atttypmod;
        dest[j]    = i;
        getTypeOutputInfo(a->atttypid, &outfunc, &typisvarlena);
        fmgr_info(outfunc, &out[j]);
        j++;
    }

    /* Column order needs no map, which dropped attributes rule out */
    bool dense = true;
    for (size_t i = 0; i < ncols; i++) {
        dense = dense && dest[i] == (int)i;
    }

    if (PG_GETARG_BOOL(4)) {
        pgch_writer_set_null_array(w, PGCH_NULL_ARRAY_EMPTY);
    }

    TableScanDesc scan   = begin_scan(rel);
    TupleTableSlot* slot = table_slot_create(rel, NULL);
    while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
        pgch_append_slot(w, slot);
    }
    table_endscan(scan);
    ExecDropSingleTupleTableSlot(slot);
    table_close(rel, AccessShareLock);

    pgch_writer_flush(w, &buf, NULL);
    bytea* bytes = bytea_from_buf(&buf);
    pgch_writer_free(w);

    reader_from_bytea(&r, &src, bytes);
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }

    void** states = palloc0(ncols * sizeof(void*));
    for (size_t i = 0; i < ncols; i++) {
        states[i] = pgch_reader_convert_init(&r, i, targets[i], typmods[i]);
    }

    while (pgch_reader_next(&r)) {
        StringInfoData row;

        memset(nulls, true, desc->natts * sizeof(bool));
        if (dense) {
            pgch_reader_fill(&r, states, values, nulls);
        } else {
            pgch_reader_fill_map(&r, states, dest, values, nulls);
        }
        initStringInfo(&row);
        for (size_t i = 0; i < ncols; i++) {
            if (i) {
                appendStringInfoChar(&row, '|');
            }
            appendStringInfoString(
                &row,
                nulls[dest[i]] ? "NULL" : OutputFunctionCall(&out[i], values[dest[i]])
            );
        }

        add_line(rows, row.data);
    }
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    pgch_reader_free(&r);

    PG_RETURN_DATUM(makeArrayResult(rows, CurrentMemoryContext));
}

/* ---- API guards ----------------------------------------------------- */

static pgch_writer*
writer_for_decls(const char* const* decls, const char* const* names, size_t n) {
    pgch_col* cols = palloc0(n * sizeof(pgch_col));

    for (size_t i = 0; i < n; i++) {
        cols[i].name     = names[i];
        cols[i].name_len = strlen(names[i]);
        cols[i].type     = parse_ch_type_cstr(decls[i], NULL);
    }
    return pgch_writer_new(CurrentMemoryContext, cols, n);
}

static pgch_writer*
writer_for_decl(const char* decl, const char* name) {
    return writer_for_decls(&decl, &name, 1);
}

static void
append_int(pgch_writer* w, size_t col, int32 v) {
    pgch_append_datum(w, col, Int32GetDatum(v), INT4OID, false);
}

static void
fault_read(pgch_chunk_source* src, text* value) {
    pgch_reader r;
    size_t rows = 0;

    pgch_reader_init_chunks(&r, src, NULL);
    while (pgch_reader_next(&r)) {
        if (r.nulls[0]) {
            elog(ERROR, "fault recovery produced NULL");
        }
        text* got        = DatumGetTextPP(r.values[0]);
        const char* want = rows < 8 ? "seed" : VARDATA(value);
        size_t len       = rows < 8 ? 4 : VARSIZE(value) - VARHDRSZ;

        if (VARSIZE_ANY_EXHDR(got) != len || memcmp(VARDATA_ANY(got), want, len) != 0) {
            elog(ERROR, "fault recovery changed decoded value");
        }
        rows++;
    }
    if (r.error || rows != 9) {
        elog(ERROR, "fault recovery changed decoded rows");
    }
    pgch_reader_free(&r);
}

PG_FUNCTION_INFO_V1(pgch_fault_probe);

Datum
pgch_fault_probe(PG_FUNCTION_ARGS) {
    char* what  = text_to_cstring(PG_GETARG_TEXT_PP(0));
    bool append = strcmp(what, "append") == 0;
    bool flush  = strcmp(what, "flush") == 0;
    bool read   = strcmp(what, "read") == 0;
    if (!append && !flush && !read) {
        elog(ERROR, "unknown fault probe: %s", what);
    }

    MemoryContext parent = CurrentMemoryContext;
    MemoryContext cxt =
        AllocSetContextCreate(parent, "fault probe", ALLOCSET_DEFAULT_SIZES);
    text* value = palloc(VARHDRSZ + 512);
    SET_VARSIZE(value, VARHDRSZ + 512);
    memset(VARDATA(value), 'x', 512);

    pgch_writer* baseline = writer_for_decl("Nullable(String)", "c");
    for (int i = 0; i < 8; i++) {
        pgch_append_datum(baseline, 0, CStringGetTextDatum("seed"), TEXTOID, false);
    }
    pgch_append_datum(baseline, 0, PointerGetDatum(value), TEXTOID, false);
    pgch_buf expected = {};
    pgch_writer_flush(baseline, &expected, NULL);
    pgch_writer_free(baseline);

    for (int nth = 1; nth < 1000; nth++) {
        MemoryContextSwitchTo(cxt);
        pgch_writer* w        = writer_for_decl("Nullable(String)", "c");
        pgch_buf* out         = palloc0(sizeof(*out));
        chunk_feed* feed      = palloc0(sizeof(*feed));
        feed->data            = expected.data;
        feed->len             = expected.len;
        feed->chunk           = 7;
        pgch_chunk_source src = { .ud = feed, .next_chunk = feed_next_chunk };
        MemoryContext rcxt =
            AllocSetContextCreate(cxt, "fault reader", ALLOCSET_DEFAULT_SIZES);
        for (int i = 0; i < 8; i++) {
            pgch_append_datum(w, 0, CStringGetTextDatum("seed"), TEXTOID, false);
        }
        pgch_checkpoint cp = {};
        pgch_writer_checkpoint(w, &cp);
        size_t saved_bytes = pgch_writer_bytes(w);
        if (!append) {
            pgch_append_datum(w, 0, PointerGetDatum(value), TEXTOID, false);
        }
        alloc_seen    = 0;
        alloc_failed  = false;
        alloc_fail_at = nth;
        PG_TRY();
        {
            if (append) {
                pgch_append_datum(w, 0, PointerGetDatum(value), TEXTOID, false);
            } else if (flush) {
                pgch_writer_flush(w, out, NULL);
            } else {
                MemoryContextSwitchTo(rcxt);
                fault_read(&src, value);
            }
        }
        PG_CATCH();
        {
            alloc_fail_at = 0;
            MemoryContextSwitchTo(cxt);
            if (!alloc_failed || geterrcode() != ERRCODE_OUT_OF_MEMORY) {
                PG_RE_THROW();
            }
            FlushErrorState();
        }
        PG_END_TRY();
        alloc_fail_at = 0;
        MemoryContextSwitchTo(cxt);

        if (alloc_failed) {
            if (append) {
                pgch_writer_rollback(w, &cp);
                if (pgch_writer_rows(w) != 8 || pgch_writer_bytes(w) != saved_bytes) {
                    elog(ERROR, "allocation failure broke rollback");
                }
                pgch_append_datum(w, 0, PointerGetDatum(value), TEXTOID, false);
            } else if (flush) {
                if (pgch_writer_rows(w) != 9) {
                    elog(ERROR, "failed flush reset writer");
                }
                pgch_buf_reset(out);
            } else {
                MemoryContextReset(rcxt);
                feed->pos   = 0;
                feed->calls = 0;
                MemoryContextSwitchTo(rcxt);
                fault_read(&src, value);
                MemoryContextSwitchTo(cxt);
            }
        }
        if (append || (flush && alloc_failed)) {
            pgch_writer_flush(w, out, NULL);
        }
        if (!read && (pgch_writer_rows(w) != 0 || out->len != expected.len ||
                      memcmp(out->data, expected.data, expected.len) != 0)) {
            elog(ERROR, "allocation failure changed serialized rows");
        }
        MemoryContextSwitchTo(parent);
        MemoryContextReset(cxt);
        if (!alloc_failed) {
            if (nth == 1) {
                elog(ERROR, "fault probe reached no allocations");
            }
            MemoryContextDelete(cxt);
            PG_RETURN_TEXT_P(cstring_to_text("ok"));
        }
    }
    elog(ERROR, "fault probe never completed");
}

PG_FUNCTION_INFO_V1(pgch_writer_probe);

/* Drive writer guards against calls no correct caller makes */
Datum
pgch_writer_probe(PG_FUNCTION_ARGS) {
    char* what         = text_to_cstring(PG_GETARG_TEXT_PP(0));
    pgch_checkpoint cp = {};
    pgch_writer* w;

    if (strcmp(what, "column_range") == 0) {
        w = writer_for_decl("Int32", "c");
        PG_RETURN_TEXT_P(cstring_to_text(psprintf(
            "kind %d scale %u",
            (int)pgch_column_kind(w, 9),
            pgch_column_datetime64_scale(w, 9)
        )));
    }
    if (strcmp(what, "array_scale") == 0) {
        w = writer_for_decl("Array(Nullable(DateTime64(3)))", "c");
        PG_RETURN_TEXT_P(
            cstring_to_text(psprintf("scale %u", pgch_column_datetime64_scale(w, 0)))
        );
    }
    if (strcmp(what, "close_idle") == 0) {
        w = writer_for_decl("Int32", "c");
        pgch_array_end(w);
        pgch_tuple_end(w);
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("nest %d", pgch_nest_active(w))));
    }
    if (strcmp(what, "array_range") == 0) {
        w = writer_for_decl("Array(Int32)", "c");
        pgch_array_begin(w, 9);
    } else if (strcmp(what, "nesting") == 0) {
        w = writer_for_decl("Array(Int32)", "c");
        pgch_array_begin(w, 0);
        pgch_tuple_end(w);
    } else if (strcmp(what, "append_range") == 0) {
        w = writer_for_decl("Int32", "c");
        append_int(w, 9, 1);
    } else if (strcmp(what, "unnamed") == 0) {
        w = writer_for_decl("Int32", "");
        pgch_append_datum(w, 0, (Datum)0, INT4OID, true);
    } else if (strcmp(what, "tuple_null") == 0) {
        w = writer_for_decl("Tuple(Int32, String)", "c");
        /* Tuple fields arrive as an array, which the library never passes as NULL */
        pgch_append_datum(w, 0, (Datum)0, ANYARRAYOID, true);
    } else if (strcmp(what, "checkpoint_nested") == 0) {
        w = writer_for_decl("Array(Int32)", "c");
        pgch_array_begin(w, 0);
        pgch_writer_checkpoint(w, &cp);
    } else if (strcmp(what, "checkpoint_stale") == 0) {
        w = writer_for_decl("Int32", "c");
        pgch_writer_checkpoint(w, &cp);
        pgch_writer_rollback(writer_for_decl("Int32", "c"), &cp);
    } else if (strcmp(what, "checkpoint_grow") == 0) {
        static const char* const decls[] = { "Int32",
                                             "Tuple(Int32, String)",
                                             "Array(Nullable(Int32))" };
        static const char* const names[] = { "a", "b", "d" };

        /* Wider writer wants more entries, so saved storage grows */
        pgch_writer_checkpoint(writer_for_decl("Int32", "c"), &cp);
        w = writer_for_decls(decls, names, lengthof(decls));
        pgch_writer_checkpoint(w, &cp);
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("entries %zu", cp.nentries)));
    } else if (strcmp(what, "rollback_built") == 0) {
        w = writer_for_decl("Int32", "c");
        append_int(w, 0, 1);
        pgch_writer_checkpoint(w, &cp);
        append_int(w, 0, 2);
        /* Block context outlives the build, rollback drops it */
        pgch_writer_build(w);
        pgch_writer_rollback(w, &cp);
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("rows %zu", pgch_writer_rows(w))));
    } else if (strcmp(what, "rebuild") == 0) {
        w = writer_for_decl("Int32", "c");
        append_int(w, 0, 1);
        pgch_writer_build(w);
        pgch_writer_build(w);
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("rows %zu", pgch_writer_rows(w))));
    } else if (strcmp(what, "bytes") == 0) {
        static const char* const decls[] = { "Int32",
                                             "String",
                                             "Nullable(Int32)",
                                             "Array(Int32)",
                                             "Tuple(Int32, String)",
                                             "LowCardinality(String)" };
        static const char* const names[] = { "f", "s", "n", "a", "t", "l" };

        w = writer_for_decls(decls, names, lengthof(decls));
        append_int(w, 0, 1);
        pgch_append_datum(w, 1, CStringGetTextDatum("s"), TEXTOID, false);
        pgch_append_datum(w, 2, (Datum)0, INT4OID, true);
        pgch_append_datum(
            w, 3, PointerGetDatum(construct_empty_array(INT4OID)), INT4ARRAYOID, false
        );
        pgch_tuple_begin(w, 4);
        append_int(w, 4, 2);
        pgch_append_datum(w, 4, CStringGetTextDatum("t"), TEXTOID, false);
        pgch_tuple_end(w);
        pgch_append_datum(w, 5, CStringGetTextDatum("l"), TEXTOID, false);
        PG_RETURN_TEXT_P(cstring_to_text(psprintf("bytes %zu", pgch_writer_bytes(w))));
    } else if (strcmp(what, "flush_long_type") == 0) {
        char label[301];
        pgch_buf out = {};

        memset(label, 'x', sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        w = writer_for_decl(psprintf("Enum8('%s'=1)", label), "c");
        pgch_writer_flush(w, &out, NULL);
    } else if (strcmp(what, "interval_unit") == 0) {
        chc_type* t  = parse_ch_type_cstr("IntervalDay", NULL);
        pgch_col col = { .name = "c", .name_len = 1, .type = t };
        Interval* iv = (Interval*)palloc0(sizeof(Interval));

        /* Every Interval the parser spells carries a unit */
        t->interval = CHC_INTERVAL_NONE;
        w           = pgch_writer_new(CurrentMemoryContext, &col, 1);
        pgch_append_datum(w, 0, IntervalPGetDatum(iv), INTERVALOID, false);
    } else if (strcmp(what, "datetime64_scale") == 0) {
        chc_type* t  = parse_ch_type_cstr("DateTime64(3)", NULL);
        pgch_col col = { .name = "c", .name_len = 1, .type = t };

        /* Parser caps precision at 9, one past the scaling table */
        t->temporal.scale = 10;
        pgch_writer_new(CurrentMemoryContext, &col, 1);
    } else {
        elog(ERROR, "unknown writer probe: %s", what);
    }
    elog(ERROR, "writer probe %s raised nothing", what);
}

PG_FUNCTION_INFO_V1(pgch_reader_probe);

/* Drive reader and value guards the block reader keeps out of reach */
Datum
pgch_reader_probe(PG_FUNCTION_ARGS) {
    char* what     = text_to_cstring(PG_GETARG_TEXT_PP(0));
    uint8_t raw[8] = {};
    Oid valtype;
    bool isnull;

    if (strcmp(what, "chunk_eof") == 0 || strcmp(what, "chunk_error") == 0) {
        bool fail           = strcmp(what, "chunk_error") == 0;
        chunk_feed feed     = { .data = raw, .chunk = 1, .fail_at = fail ? 1 : 0 };
        pgch__chunks chunks = {
            .src = { .ud = &feed, .next_chunk = feed_next_chunk },
            .cxt = CurrentMemoryContext,
        };
        chc_err err = {};
        size_t n    = 1;
        int rc      = pgch__chunk_read(&chunks, raw, sizeof(raw), &n, &err);

        if (rc != (fail ? CHC_ERR_IO : CHC_OK) || !chunks.eos ||
            (fail ? chunks.error == NULL : n != 0)) {
            elog(ERROR, "unexpected chunk termination");
        }
        if (fail && pgch__chunk_next_block(&chunks) != NULL) {
            elog(ERROR, "chunk callback resumed after error");
        }
        n = 1;
        if (pgch__chunk_read(&chunks, raw, sizeof(raw), &n, &err) != CHC_OK || n != 0 ||
            feed.calls != 1) {
            elog(ERROR, "chunk callback resumed source after termination");
        }
        PG_RETURN_TEXT_P(cstring_to_text("ok"));
    }
    if (strcmp(what, "read_datetime64_scale") == 0 ||
        strcmp(what, "read_time64_scale") == 0) {
        bool time64    = strcmp(what, "read_time64_scale") == 0;
        chc_column col = chc_build_fixed(raw, sizeof raw, 1);
        chc_type* t = parse_ch_type_cstr(time64 ? "Time64(3)" : "DateTime64(3)", NULL);

        /* Parser caps precision at 9, one past the scaling table */
        t->temporal.scale = 10;
        pgch_read_value(&col, t, 0, &valtype, &isnull);
    } else if (strcmp(what, "read_unsupported") == 0) {
        chc_column col = chc_build_fixed(raw, sizeof raw, 1);

        /* Reader rejects the column before reading rows, callers may not */
        pgch_read_value(
            &col, parse_ch_type_cstr("Dynamic", NULL), 0, &valtype, &isnull
        );
    } else if (strcmp(what, "read_empty_tuple") == 0) {
        chc_column col = chc_build_fixed(raw, sizeof raw, 1);

        pgch_read_value(
            &col, parse_ch_type_cstr("Tuple()", NULL), 0, &valtype, &isnull
        );
    } else if (strcmp(what, "read_lc_key_size") == 0) {
        uint64_t offs[1] = { 1 };
        chc_column dict  = chc_build_string(offs, (const uint8_t*)"a", 1);
        /* Wire spells four key widths, the builder takes any */
        chc_column col = chc_build_lc(3, raw, 1, &dict);

        pgch_read_value(
            &col,
            parse_ch_type_cstr("LowCardinality(String)", NULL),
            0,
            &valtype,
            &isnull
        );
    } else if (strcmp(what, "read_lc_inner") == 0) {
        chc_column dict = chc_build_fixed(raw, 2, 1);
        chc_column col  = chc_build_lc(4, raw, 1, &dict);

        pgch_read_value(
            &col,
            parse_ch_type_cstr("LowCardinality(FixedString(2))", NULL),
            0,
            &valtype,
            &isnull
        );
    } else if (strcmp(what, "read_decimal_width") == 0) {
        /* Columns off the wire carry a width the digit formatter can take */
        chc_column col = chc_build_fixed(raw, 5, 1);

        pgch_read_value(
            &col, parse_ch_type_cstr("Decimal128(2)", NULL), 0, &valtype, &isnull
        );
    } else if (strcmp(what, "read_wide_width") == 0) {
        chc_column col = chc_build_fixed(raw, 5, 1);

        pgch_read_value(&col, parse_ch_type_cstr("Int128", NULL), 0, &valtype, &isnull);
    } else if (strcmp(what, "read_decimal_scale") == 0) {
        uint8_t wide[32] = {};
        chc_column col   = chc_build_fixed(wide, sizeof wide, 1);
        chc_type* t      = parse_ch_type_cstr("Decimal256(76)", NULL);

        /* Parser caps scale at the declared precision, 76 digits */
        t->decimal.scale = 79;
        pgch_read_value(&col, t, 0, &valtype, &isnull);
    } else if (strcmp(what, "read_many_points") == 0) {
        uint64_t offs[1]   = { (uint64_t)INT_MAX };
        chc_column axes[2] = { chc_build_fixed(raw, 8, 1), chc_build_fixed(raw, 8, 1) };
        chc_column* pair[2] = { &axes[0], &axes[1] };
        chc_column point    = chc_build_tuple(pair, 2);
        chc_column col      = chc_build_array(offs, 1, &point);

        pgch_read_value(&col, parse_ch_type_cstr("Ring", NULL), 0, &valtype, &isnull);
    } else if (strcmp(what, "read_array_item") == 0) {
        uint64_t offs[1] = { 1 };
        chc_column inner = chc_build_fixed(raw, 1, 1);
        chc_column col   = chc_build_array(offs, 1, &inner);

        pgch_read_value(
            &col, parse_ch_type_cstr("Array(Nothing)", NULL), 0, &valtype, &isnull
        );
    } else {
        elog(ERROR, "unknown reader probe: %s", what);
    }
    elog(ERROR, "reader probe %s raised nothing", what);
}

PG_FUNCTION_INFO_V1(pgch_decode_first);

/* Decode the first row, then release the reader while its block still holds rows */
Datum
pgch_decode_first(PG_FUNCTION_ARGS) {
    bytes_source src;
    pgch_reader r;
    char* out = NULL;

    reader_from_bytea(&r, &src, PG_GETARG_BYTEA_PP(0));
    if (pgch_reader_convert_init(&r, pgch_reader_columns(&r), INT4OID, -1)) {
        elog(ERROR, "prepared conversion for a column past the block");
    }
    if (pgch_reader_next(&r)) {
        out = pgch_value_to_cstring(
            chc_block_column_type(r.cur, 0), r.values[0], r.encoding_check
        );
    }
    pgch_reader_free(&r);

    if (!out) {
        PG_RETURN_NULL();
    }
    PG_RETURN_TEXT_P(cstring_to_text(out));
}

static const chc_block*
failed_next_block(void* ud pg_attribute_unused()) {
    return NULL;
}

static const char*
failed_source_error(void* ud pg_attribute_unused()) {
    return "source failed before first block";
}

PG_FUNCTION_INFO_V1(pgch_decode_failed_source);

/* Report a source that fails before the reader asks for a block */
Datum
pgch_decode_failed_source(PG_FUNCTION_ARGS pg_attribute_unused()) {
    pgch_block_source bsrc = { .ud         = NULL,
                               .next_block = failed_next_block,
                               .error      = failed_source_error };
    pgch_reader r;

    pgch_reader_init(&r, &bsrc);
    if (!r.error) {
        elog(ERROR, "reader missed the source error");
    }
    PG_RETURN_TEXT_P(cstring_to_text(r.error));
}

PG_FUNCTION_INFO_V1(pgch_decode_typed_decl);

/* Decode rows with conversion prepared from a ClickHouse declaration */
Datum
pgch_decode_typed_decl(PG_FUNCTION_ARGS) {
    Oid outtype     = get_fn_expr_argtype(fcinfo->flinfo, 2);
    int32 outtypmod = arg_typmod(fcinfo, 2);
    chc_type* t     = parse_ch_type(PG_GETARG_TEXT_PP(1), "column c");
    ArrayBuildState* out;
    FmgrInfo outfn = {};
    Oid outfuncid;
    bool typisvarlena;
    bytes_source src;
    pgch_reader r;
    void* state;

    if (!OidIsValid(outtype)) {
        elog(ERROR, "could not determine target type");
    }
    getTypeOutputInfo(outtype, &outfuncid, &typisvarlena);
    fmgr_info(outfuncid, &outfn);
    out = initArrayResult(TEXTOID, CurrentMemoryContext, false);

    reader_from_bytea(&r, &src, PG_GETARG_BYTEA_PP(0));
    state = pgch_convert_init_type(t, outtype, outtypmod, r.encoding_check);
    while (pgch_reader_next(&r)) {
        Datum val = (Datum)0;

        if (!r.nulls[0]) {
            val = CStringGetTextDatum(
                OutputFunctionCall(&outfn, pgch_convert(state, r.values[0]))
            );
        }
        accumArrayResult(out, val, r.nulls[0], TEXTOID, CurrentMemoryContext);
    }
    if (r.error) {
        elog(ERROR, "decode: %s", r.error);
    }
    pgch_convert_free(state);
    pgch_reader_free(&r);

    PG_RETURN_DATUM(makeArrayResult(out, CurrentMemoryContext));
}
