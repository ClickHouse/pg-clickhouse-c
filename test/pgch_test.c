/*
 * Expose pg-clickhouse-c through SQL tests
 *
 * Run Native round trips in memory without ClickHouse or chDB
 * Compile both header implementations here
 */

#include "postgres.h"

#include <ctype.h>
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

#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

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

/* Decode first column as text, optionally prepare conversion from column type */
static Datum
decode_reader(pgch_reader* r, Oid outtype, int32 outtypmod, bool from_type) {
    ArrayBuildState* out = initArrayResult(TEXTOID, CurrentMemoryContext, false);
    void* convstate      = NULL;
    bool converted       = false;
    FmgrInfo outfn       = {};

    if (OidIsValid(outtype)) {
        Oid outfuncid;
        bool varlena;

        getTypeOutputInfo(outtype, &outfuncid, &varlena);
        fmgr_info(outfuncid, &outfn);
    }

    if (r->error) {
        elog(ERROR, "decode: %s", r->error);
    }
    if (pgch_reader_columns(r) != 1) {
        elog(ERROR, "expected 1 column, got %zu", pgch_reader_columns(r));
    }
    if (OidIsValid(outtype) && from_type) {
        convstate = pgch_reader_convert_init(r, 0, outtype, outtypmod);
        converted = true;
    }

    while (pgch_reader_next(r)) {
        bool isnull = r->nulls[0];
        Datum val   = (Datum)0;

        if (isnull) {
        } else if (!OidIsValid(outtype)) {
            val = CStringGetTextDatum(
                pgch_value_to_cstring(r->coltypes[0], r->values[0])
            );
        } else {
            if (!converted) {
                convstate =
                    pgch_convert_init(r->values[0], r->coltypes[0], outtype, outtypmod);
                converted = true;
            }
            val = CStringGetTextDatum(
                OutputFunctionCall(&outfn, pgch_convert(convstate, r->values[0]))
            );
        }
        accumArrayResult(out, val, isnull, TEXTOID, CurrentMemoryContext);
    }
    if (r->error) {
        elog(ERROR, "decode: %s", r->error);
    }
    if (convstate) {
        pgch_convert_free(convstate);
    }
    pgch_reader_free(r);

    return makeArrayResult(out, CurrentMemoryContext);
}

static Datum
decode_column(bytea* data, Oid outtype, int32 outtypmod, bool from_type) {
    bytes_source src;
    pgch_reader r;

    reader_from_bytea(&r, &src, data);
    return decode_reader(&r, outtype, outtypmod, from_type);
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

/* Supply fixed-size chunks */
typedef struct {
    const uint8_t* data;
    size_t len;
    size_t pos;
    size_t chunk;
} chunk_feed;

static bool
feed_next_chunk(
    void* ud,
    const void** p,
    size_t* n,
    char** error pg_attribute_unused()
) {
    chunk_feed* f = (chunk_feed*)ud;
    size_t take   = Min(f->chunk, f->len - f->pos);

    *p = f->data + f->pos;
    *n = take;
    f->pos += take;
    return true;
}

PG_FUNCTION_INFO_V1(pgch_decode_chunks);

/* Decode Native bytes through chunk source */
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
    PG_RETURN_DATUM(decode_reader(&r, InvalidOid, -1, false));
}

PG_FUNCTION_INFO_V1(pgch_decode);

/* Decode first column of every row as text */
Datum
pgch_decode(PG_FUNCTION_ARGS) {
    PG_RETURN_DATUM(decode_column(PG_GETARG_BYTEA_PP(0), InvalidOid, -1, false));
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
        decode_column(PG_GETARG_BYTEA_PP(0), outtype, arg_typmod(fcinfo, 1), false)
    );
}

PG_FUNCTION_INFO_V1(pgch_decode_typed);

/* Decode rows with conversion prepared from ClickHouse column type */
Datum
pgch_decode_typed(PG_FUNCTION_ARGS) {
    Oid outtype = get_fn_expr_argtype(fcinfo->flinfo, 1);

    if (PG_ARGISNULL(0)) {
        PG_RETURN_NULL();
    }
    if (!OidIsValid(outtype)) {
        elog(ERROR, "could not determine target type");
    }
    PG_RETURN_DATUM(
        decode_column(PG_GETARG_BYTEA_PP(0), outtype, arg_typmod(fcinfo, 1), true)
    );
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
    { "JSON", NULL, NULL, NULL, "Also reads into json" },
    { "LowCardinality", "T", "String", "T" },
    { "Map", "K,V", "String, Int64", NULL, "One record per pair" },
    { "Nested", NULL, "a Int32" },
    { "Nullable", "T", "Int32", "T", "Sets nullable on the column" },
    { "Object",
     NULL, "'json'",
     NULL, NULL,
     "serializes as a materialized Tuple, unlike JSON" },
    { "QBit", NULL, "BFloat16, 16" },
    { "SimpleAggregateFunction", NULL, "sum, Int64" },
    { "String", NULL, NULL, NULL, "Also reads into bytea" },
    { "Time64", "P", "3", "time(P) without time zone", "P over 6 caps at 6" },
    { "Tuple", "...", "Int32, String", NULL, "Pseudo type, no column takes it" },
    { "UInt64", NULL, NULL, NULL, "Errors on values > BIGINT max" },
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
    int cap     = CHC__NAME_TABLE_M + lengthof(type_docs);
    type_scan s = {
        .rows    = palloc0(cap * sizeof(char*)),
        .omitted = palloc0(cap * sizeof(char*)),
    };

    for (unsigned i = 0; i < CHC__NAME_TABLE_M; i++) {
        if (chc__name_table[i].name) {
            scan_type(&s, chc__name_table[i].name);
        }
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
    char* s             = pgch_structure_from_tupdesc(RelationGetDescr(rel), &opts);

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
        bool varlena;

        if (!pgch_attr_is_streamed(a)) {
            continue;
        }
        targets[j] = a->atttypid;
        typmods[j] = a->atttypmod;
        dest[j]    = i;
        getTypeOutputInfo(a->atttypid, &outfunc, &varlena);
        fmgr_info(outfunc, &out[j]);
        j++;
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
        pgch_reader_fill_map(&r, states, dest, values, nulls);
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
