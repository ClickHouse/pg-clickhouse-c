/*
 * PostgreSQL bindings shared by pg-clickhouse encoders and decoders
 *
 * Define PGCH_IMPLEMENTATION in one translation unit. Include this header
 * without that definition everywhere else. Consumers provide transport,
 * block framing, and query execution
 */

#ifndef PG_CLICKHOUSE_H
#define PG_CLICKHOUSE_H

#include "postgres.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "access/tupdesc.h"
#include "utils/palloc.h"

#include "clickhouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PostgreSQL added pg_noreturn in version 18 */
#ifndef pg_noreturn
#define pg_noreturn pg_attribute_noreturn()
#endif

/* ---- errors --------------------------------------------------------- */

/*
 * Define as a string literal in build flags to prefix every reported error
 * Leave undefined for no prefix
 */
#ifndef PGCH_MSG_PREFIX
#define PGCH_MSG_PREFIX ""
#endif

#define pgch_error(sqlstate, msg)                                                      \
    ereport(ERROR, errcode(sqlstate), errmsg(PGCH_MSG_PREFIX "%s", msg))

#define pgch_errorf(sqlstate, fmt, ...)                                                \
    ereport(ERROR, errcode(sqlstate), errmsg(PGCH_MSG_PREFIX fmt, __VA_ARGS__))

/* Raise ERROR with `what` inserted before clickhouse-c error message */
pg_noreturn extern void
pgch_raise(const chc_err* err, int sqlstate, const char* what);

/* ---- allocator ------------------------------------------------------ */

/*
 * Allocate clickhouse-c data in CurrentMemoryContext
 * Switch contexts before calling clickhouse-c to control allocation lifetime
 */
extern const chc_alloc pgch_alloc;

/*
 * Return a zeroed input parser
 * Available when implementation translation unit defines CHC_IMPLEMENTATION
 */
extern chc_in*
pgch_in_alloc(void);

/* ---- type mapping --------------------------------------------------- */

/* Map scalar ClickHouse kinds to PostgreSQL type OIDs */
extern const Oid pgch_kind_oids[CHC_KIND_COUNT];

/* Provide powers of ten for supported DateTime64 and Time64 scales */
extern const int64_t pgch_pow10[10];

/*
 * Return OID describing Datum produced by pgch_read_value
 * Return ANYARRAYOID for pgch_array and RECORDOID for pgch_tuple
 */
extern Oid
pgch_datum_oid(const chc_type* type);

/*
 * Return PostgreSQL column type for a ClickHouse type
 * Resolve Array to PostgreSQL array type for its leaf element
 */
extern Oid
pgch_native_oid(const chc_type* type);

/*
 * Return pgch_native_oid result and add `what` to unsupported-type errors
 * Pass NULL to omit context
 */
extern Oid
pgch_native_oid_for(const chc_type* type, const char* what);

/* Remove Nullable and LowCardinality wrappers, report detected nullability */
extern const chc_type*
pgch_unwrap(const chc_type* type, bool* out_nullable);

/* ---- PG type -> CH type name ---------------------------------------- */

/* Configure optional PostgreSQL to ClickHouse type mappings */
typedef struct pgch_type_opts {
    bool json_as_json;      /* Map json and jsonb to JSON instead of String */
    bool low_cardinality;   /* Wrap String columns in LowCardinality */
    bool numeric_as_string; /* Map unconstrained numeric to String */
} pgch_type_opts;

/*
 * Return palloc'd ClickHouse type declaration for a PostgreSQL column
 * Include required Nullable wrapper, pass NULL opts for defaults
 */
extern char*
pgch_ch_type_for(Oid typid, int32 typmod, bool notnull, const pgch_type_opts* opts);

/* Return palloc'd ClickHouse identifier, quoted when needed */
extern char*
pgch_quote_ch_ident(const char* name);

/* Return true when attribute carries a value in a Native stream */
static inline bool
pgch_attr_is_streamed(Form_pg_attribute attr) {
    return !attr->attisdropped && !attr->attgenerated;
}

/* Return palloc'd `name type, ...` declaration for streamed attributes */
extern char*
pgch_structure_from_tupdesc(TupleDesc desc, const pgch_type_opts* opts);

/* ---- server settings ------------------------------------------------ */

/* Apply to ClickHouse queries that return Native data for this library */
#define PGCH_NATIVE_SETTINGS                                                           \
    "output_format_native_encode_types_in_binary_format=0,"                            \
    "output_format_native_write_json_as_string=1"

/* Use for chDB and clickhouse-local Native framing */
extern const chc_block_opts pgch_block_opts_local;

/* ---- Intermediate Array / Tuple representations --------------------- */

/*
 * Represent decoded or staged arrays
 * For ndim greater than one, each datum points to a child pgch_array
 */
typedef struct pgch_array {
    Datum* datums;
    bool* nulls;
    size_t len;
    int ndim;       /* Nesting depth, at least one */
    Oid item_type;  /* PostgreSQL leaf type */
    Oid array_type; /* PostgreSQL array type */
} pgch_array;

/* Represent a decoded ClickHouse Tuple */
typedef struct pgch_tuple {
    Datum* datums;
    bool* nulls;
    Oid* types;
    size_t len;
    const char* ch_type_name;
} pgch_tuple;

/* ---- byte buffer ---------------------------------------------------- */

/* Store growable bytes in CurrentMemoryContext */
typedef struct pgch_buf {
    uint8_t* data;
    size_t len;
    size_t cap;
} pgch_buf;

extern void
pgch_buf_reserve(pgch_buf* b, size_t need);
extern void
pgch_buf_append(pgch_buf* b, const void* src, size_t n);
extern void
pgch_buf_append_zero(pgch_buf* b, size_t n);
extern void
pgch_buf_reset(pgch_buf* b);

/* Initialize a write-only chc_io that appends to b, b must outlive out_io */
extern void
pgch_buf_io(pgch_buf* b, chc_io* out_io);

#ifdef PGCH_IMPLEMENTATION

#include <string.h>

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "utils/lsyscache.h"

/* ClickHouse uses Unix epoch, PostgreSQL uses 2000-01-01 */
#define PGCH__DATE_OFFSET (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE)

/* ---- errors --------------------------------------------------------- */

void
pgch_raise(const chc_err* err, int sqlstate, const char* what) {
    const char* m = (err && err->msg[0]) ? err->msg : "unknown error";

    ereport(
        ERROR, errcode(sqlstate), errmsg(PGCH_MSG_PREFIX "%s%s", what ? what : "", m)
    );
}

/* ---- allocator ------------------------------------------------------ */

static void*
pgch__alloc(void* ud pg_attribute_unused(), size_t n) {
    return MemoryContextAllocExtended(CurrentMemoryContext, n, MCXT_ALLOC_HUGE);
}

static void*
pgch__realloc(
    void* ud pg_attribute_unused(),
    void* p,
    size_t old_bytes pg_attribute_unused(),
    size_t new_bytes
) {
    if (!p) {
        return MemoryContextAllocExtended(
            CurrentMemoryContext, new_bytes, MCXT_ALLOC_HUGE
        );
    }
    return repalloc_huge(p, new_bytes);
}

static void
pgch__free(
    void* ud pg_attribute_unused(),
    void* p,
    size_t bytes pg_attribute_unused()
) {
    if (p) {
        pfree(p);
    }
}

const chc_alloc pgch_alloc = {
    .ud      = NULL,
    .alloc   = pgch__alloc,
    .realloc = pgch__realloc,
    .free    = pgch__free,
};

#ifdef CHC_IMPLEMENTATION
chc_in*
pgch_in_alloc(void) {
    return palloc0(sizeof(chc_in));
}
#endif

/* ---- type mapping --------------------------------------------------- */

const Oid pgch_kind_oids[CHC_KIND_COUNT] = {
    [CHC_INT8]         = INT2OID,
    [CHC_INT16]        = INT2OID,
    [CHC_UINT8]        = INT2OID,
    [CHC_BOOL]         = BOOLOID,
    [CHC_INT32]        = INT4OID,
    [CHC_UINT16]       = INT4OID,
    [CHC_INT64]        = INT8OID,
    [CHC_UINT32]       = INT8OID,
    [CHC_UINT64]       = INT8OID,
    [CHC_FLOAT32]      = FLOAT4OID,
    [CHC_FLOAT64]      = FLOAT8OID,
    [CHC_DECIMAL32]    = NUMERICOID,
    [CHC_DECIMAL64]    = NUMERICOID,
    [CHC_DECIMAL128]   = NUMERICOID,
    [CHC_DECIMAL256]   = NUMERICOID,
    [CHC_STRING]       = TEXTOID,
    [CHC_FIXED_STRING] = TEXTOID,
    [CHC_ENUM8]        = TEXTOID,
    [CHC_ENUM16]       = TEXTOID,
    [CHC_JSON]         = JSONBOID,
    [CHC_OBJECT]       = JSONBOID,
    [CHC_DATE]         = DATEOID,
    [CHC_DATE32]       = DATEOID,
    [CHC_DATETIME]     = TIMESTAMPTZOID,
    [CHC_DATETIME64]   = TIMESTAMPTZOID,
    [CHC_TIME]         = TIMEOID,
    [CHC_TIME64]       = TIMEOID,
    [CHC_UUID]         = UUIDOID,
    [CHC_IPV4]         = INETOID,
    [CHC_IPV6]         = INETOID,
};

const int64_t pgch_pow10[10] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
};

static const char*
pgch__where(const char* what) {
    return what ? psprintf(" (%s)", what) : "";
}

static Oid
pgch__datum_oid(const chc_type* type, const char* what) {
    chc_kind kind = chc_type_kind(type);
    Oid oid       = pgch_kind_oids[kind];

    if (OidIsValid(oid)) {
        return oid;
    }

    switch (kind) {
    case CHC_VOID:
    case CHC_NOTHING:
        return InvalidOid;
    case CHC_NULLABLE:
    case CHC_LOW_CARDINALITY:
        return pgch__datum_oid(chc_type_child(type, 0), what);
    case CHC_ARRAY:
        return ANYARRAYOID;
    case CHC_TUPLE:
        return RECORDOID;
    default:
        pgch_errorf(
            ERRCODE_FDW_INVALID_DATA_TYPE,
            "unsupported column type \"%s\"%s",
            chc_type_name(type, NULL),
            pgch__where(what)
        );
    }
    pg_unreachable();
}

Oid
pgch_datum_oid(const chc_type* type) {
    return pgch__datum_oid(type, NULL);
}

Oid
pgch_native_oid_for(const chc_type* type, const char* what) {
    chc_kind kind = chc_type_kind(type);
    Oid oid       = pgch_kind_oids[kind];

    if (OidIsValid(oid)) {
        return oid;
    }

    switch (kind) {
    case CHC_NULLABLE:
    case CHC_LOW_CARDINALITY:
        return pgch_native_oid_for(chc_type_child(type, 0), what);
    case CHC_ARRAY: {
        /* PostgreSQL uses one array type for every nesting depth */
        const chc_type* leaf = type;
        Oid array_type;

        while (chc_type_kind(leaf) == CHC_ARRAY) {
            leaf = chc_type_child(leaf, 0);
        }
        array_type = get_array_type(pgch_native_oid_for(leaf, what));
        if (!OidIsValid(array_type)) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE,
                "no PG array type for \"%s\"%s",
                chc_type_name(leaf, NULL),
                pgch__where(what)
            );
        }
        return array_type;
    }
    default:
        return pgch__datum_oid(type, what);
    }
}

Oid
pgch_native_oid(const chc_type* type) {
    return pgch_native_oid_for(type, NULL);
}

const chc_type*
pgch_unwrap(const chc_type* type, bool* out_nullable) {
    bool nullable = false;

    if (chc_type_kind(type) == CHC_NULLABLE) {
        nullable = true;
        type     = chc_type_child(type, 0);
    }
    if (chc_type_kind(type) == CHC_LOW_CARDINALITY) {
        type = chc_type_child(type, 0);
        /* ClickHouse nests Nullable inside LowCardinality */
        if (chc_type_kind(type) == CHC_NULLABLE) {
            nullable = true;
            type     = chc_type_child(type, 0);
        }
    }
    if (out_nullable) {
        *out_nullable = nullable;
    }
    return type;
}

/* ---- PG type -> CH type name ---------------------------------------- */

const chc_block_opts pgch_block_opts_local = {};

static bool
pgch__numeric_typmod(int32 typmod, int* precision, int* scale) {
    if (typmod < (int32)VARHDRSZ) {
        return false;
    }
    *precision = (int)(((typmod - VARHDRSZ) >> 16) & 0xffff);
#if PG_VERSION_NUM >= 150000
    /* PostgreSQL 15 stores signed numeric scale with a 1024 bias */
    *scale = (int)((((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024);
#else
    *scale = (int)((typmod - VARHDRSZ) & 0xffff);
#endif
    return true;
}

static const char*
pgch__ch_scalar(Oid typid, int32 typmod, const pgch_type_opts* opts) {
    switch (typid) {
    case BOOLOID:
        return "Bool";
    case INT2OID:
        return "Int16";
    case INT4OID:
        return "Int32";
    case INT8OID:
        return "Int64";
    case OIDOID:
        return "UInt32";
    case XID8OID:
#if PG_VERSION_NUM >= 190000
    case OID8OID:
#endif
        return "UInt64";
    case FLOAT4OID:
        return "Float32";
    case FLOAT8OID:
        return "Float64";
    case NUMERICOID: {
        int precision, scale;

        /* ClickHouse Decimal supports up to 76 digits and no negative scale */
        if (pgch__numeric_typmod(typmod, &precision, &scale) && precision >= 1 &&
            precision <= 76 && scale >= 0 && scale <= precision) {
            return psprintf("Decimal(%d,%d)", precision, scale);
        }
        return opts->numeric_as_string ? NULL : "Decimal256(38)";
    }
    case UUIDOID:
        return "UUID";
    case DATEOID:
        /* ClickHouse Date cannot represent dates before 1970 */
        return "Date32";
    case TIMEOID:
        return "Time64(6)";
    case TIMESTAMPOID:
        /* Native stores DateTime64 as an integer without session timezone */
        return "DateTime64(6)";
    case TIMESTAMPTZOID:
        return "DateTime64(6, 'UTC')";
    case JSONOID:
    case JSONBOID:
        return opts->json_as_json ? "JSON" : NULL;
    default:
        /*
         * FixedString counts bytes while PostgreSQL character typmods count
         * characters. String also provides common representation for types
         * without matching Native column types
         */
        return NULL;
    }
}

char*
pgch_ch_type_for(Oid typid, int32 typmod, bool notnull, const pgch_type_opts* opts) {
    static const pgch_type_opts defaults = {};
    Oid elemtype;
    const char* base;

    if (!opts) {
        opts = &defaults;
    }
    typid = getBaseTypeAndTypmod(typid, &typmod);

    /* ClickHouse rejects Nullable(Array), apply nullability to elements */
    elemtype = get_element_type(typid);
    if (OidIsValid(elemtype)) {
        return psprintf("Array(%s)", pgch_ch_type_for(elemtype, typmod, notnull, opts));
    }

    base = pgch__ch_scalar(typid, typmod, opts);
    if (!base) {
        if (opts->low_cardinality) {
            return psprintf(
                "LowCardinality(%s)", notnull ? "String" : "Nullable(String)"
            );
        }
        base = "String";
    }
    /* ClickHouse rejects Nullable(JSON), JSON already represents null */
    if (notnull || strcmp(base, "JSON") == 0) {
        return pstrdup(base);
    }
    return psprintf("Nullable(%s)", base);
}

char*
pgch_quote_ch_ident(const char* name) {
    bool bare = name[0] != '\0';
    StringInfoData buf;

    for (const char* p = name; bare && *p; p++) {
        bare = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ||
               (p != name && *p >= '0' && *p <= '9');
    }
    if (bare) {
        return pstrdup(name);
    }

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '"');
    for (const char* p = name; *p; p++) {
        /* ClickHouse quoted identifiers escape quotes and backslashes */
        if (*p == '"' || *p == '\\') {
            appendStringInfoChar(&buf, *p == '"' ? '"' : '\\');
        }
        appendStringInfoChar(&buf, *p);
    }
    appendStringInfoChar(&buf, '"');
    return buf.data;
}

char*
pgch_structure_from_tupdesc(TupleDesc desc, const pgch_type_opts* opts) {
    StringInfoData buf;

    initStringInfo(&buf);
    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);

        if (!pgch_attr_is_streamed(a)) {
            continue;
        }
        if (buf.len) {
            appendStringInfoString(&buf, ", ");
        }
        appendStringInfo(
            &buf,
            "%s %s",
            pgch_quote_ch_ident(NameStr(a->attname)),
            pgch_ch_type_for(a->atttypid, a->atttypmod, a->attnotnull, opts)
        );
    }
    return buf.data;
}

/* ---- byte buffer ---------------------------------------------------- */

void
pgch_buf_reserve(pgch_buf* b, size_t need) {
    if (need <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap : 64;

    while (ncap < need) {
        ncap = ncap > SIZE_MAX / 2 ? need : ncap * 2;
    }
    b->data =
        b->data
            ? repalloc_huge(b->data, ncap)
            : MemoryContextAllocExtended(CurrentMemoryContext, ncap, MCXT_ALLOC_HUGE);
    b->cap = ncap;
}

void
pgch_buf_append(pgch_buf* b, const void* src, size_t n) {
    pgch_buf_reserve(b, b->len + n);
    if (src && n) {
        memcpy(b->data + b->len, src, n);
    }
    b->len += n;
}

void
pgch_buf_append_zero(pgch_buf* b, size_t n) {
    pgch_buf_reserve(b, b->len + n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}

void
pgch_buf_reset(pgch_buf* b) {
    b->len = 0;
}

static int
pgch__buf_write(void* ud, const void* buf, size_t len, chc_err* err) {
    pgch_buf_append((pgch_buf*)ud, buf, len);
    (void)err;
    return CHC_OK;
}

void
pgch_buf_io(pgch_buf* b, chc_io* out_io) {
    *out_io = (chc_io){ .ud = b, .write = pgch__buf_write };
}

#endif /* PGCH_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* PG_CLICKHOUSE_H */
