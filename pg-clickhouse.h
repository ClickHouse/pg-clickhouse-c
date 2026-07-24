/*
 * pg-clickhouse.h -- PostgreSQL bindings for clickhouse-c, core header.
 *
 * palloc-backed chc_alloc, chc_err -> ereport mapping, CH type to PG type
 * OID mapping, a growable buffer doubling as a chc_io write sink, and the
 * intermediate Array / Tuple carriers that decode and encode share.
 *
 * Exactly one TU must `#define PGCH_IMPLEMENTATION` before including; other
 * TUs include for declarations only. Transport, block framing and query
 * execution stay with the caller: nothing here touches a socket, a chc_client
 * or a chdb handle.
 */

#ifndef PG_CLICKHOUSE_H
#define PG_CLICKHOUSE_H

#include "postgres.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/palloc.h"

#include "clickhouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pg_noreturn arrived in PG 18. */
#ifndef pg_noreturn
#define pg_noreturn pg_attribute_noreturn()
#endif

/* ---- errors --------------------------------------------------------- */

/*
 * Prepended to every message this library ereports. Point at a literal that
 * outlives the backend, eg "pg_chdb: ". Empty by default.
 */
extern const char* pgch_msg_prefix;

#define pgch_error(sqlstate, msg)                                                      \
    ereport(ERROR, errcode(sqlstate), errmsg("%s%s", pgch_msg_prefix, msg))

#define pgch_errorf(sqlstate, fmt, ...)                                                \
    ereport(ERROR, errcode(sqlstate), errmsg("%s" fmt, pgch_msg_prefix, __VA_ARGS__))

/* ereport ERROR carrying err->msg, with `what` between prefix and message. */
pg_noreturn extern void
pgch_raise(const chc_err* err, int sqlstate, const char* what);

/* ---- allocator ------------------------------------------------------ */

/*
 * palloc on CurrentMemoryContext at every call, MCXT_ALLOC_HUGE so decoded
 * block buffers can exceed the 1GB cap. Callers control placement by
 * switching context before entering clickhouse-c.
 */
extern const chc_alloc pgch_alloc;

/*
 * Zeroed chc_in for the caller's read loop. sizeof(chc_in) is visible only
 * where clickhouse-c's implementation is compiled, so this is defined only
 * when the PGCH_IMPLEMENTATION TU also carries CHC_IMPLEMENTATION.
 */
extern chc_in*
pgch_in_alloc(void);

/* ---- type mapping --------------------------------------------------- */

/* Scalar CH kind -> PG type OID; InvalidOid for wrapper and unsupported kinds. */
extern const Oid pgch_kind_oids[CHC_KIND_COUNT];

/* Power-of-10 lookup; CH bounds DateTime64 and Decimal scale to [0, 9]. */
extern const int64_t pgch_pow10[10];

/*
 * OID describing the Datum pgch_read_value produces: scalars map through
 * pgch_kind_oids, Array yields ANYARRAYOID (a pgch_array*), Tuple yields
 * RECORDOID (a pgch_tuple*). Nullable and LowCardinality are transparent.
 */
extern Oid
pgch_datum_oid(const chc_type* type);

/*
 * Real PG type for a CH column: as pgch_datum_oid, except Array resolves to
 * the PG array type of its leaf element. Use when declaring a TupleDesc.
 */
extern Oid
pgch_native_oid(const chc_type* type);

/*
 * Strip Nullable, then LowCardinality and any Nullable inside it. Sets
 * *out_nullable when either layer was present.
 */
extern const chc_type*
pgch_unwrap(const chc_type* type, bool* out_nullable);

/* ---- Array / Tuple carriers ----------------------------------------- */

/*
 * An Array decoded from CH or staged for encoding. For nested arrays ndim > 1
 * and datums[i] points at a child pgch_array with ndim - 1. item_type is the
 * leaf scalar PG type; array_type is the PG array type, identical at every
 * depth because PG has one array type per element type regardless of ndim
 * (InvalidOid when built for encoding, where it is unused).
 */
typedef struct pgch_array {
    Datum* datums;
    bool* nulls;
    size_t len;
    int ndim;       /* nesting depth, >= 1 */
    Oid item_type;  /* leaf scalar PG type */
    Oid array_type; /* PG array type */
} pgch_array;

/* A Tuple decoded from CH. types[i] is the OID of datums[i], per pgch_datum_oid. */
typedef struct pgch_tuple {
    Datum* datums;
    bool* nulls;
    Oid* types;
    size_t len;
    const char* ch_type_name; /* for error messages */
} pgch_tuple;

/* ---- byte buffer ---------------------------------------------------- */

/*
 * Growable palloc'd byte buffer. Grows against CurrentMemoryContext, so
 * switch before the first append. Freed by deleting that context.
 */
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

/*
 * Fill *out_io with a write-only chc_io appending to b, for handing
 * chc_block_write output to an in-memory consumer. read and check_cancel
 * are NULL; b must outlive out_io.
 */
extern void
pgch_buf_io(pgch_buf* b, chc_io* out_io);

#ifdef PGCH_IMPLEMENTATION

#include "catalog/pg_type_d.h"
#include "datatype/timestamp.h"
#include "utils/lsyscache.h"

/* CH Date / Date32 / DateTime epoch is unix; offset to PG epoch (2000-01-01) */
#define PGCH__DATE_OFFSET (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE)

const char* pgch_msg_prefix = "";

/* ---- errors --------------------------------------------------------- */

void
pgch_raise(const chc_err* err, int sqlstate, const char* what) {
    const char* m = (err && err->msg[0]) ? err->msg : "unknown error";

    ereport(
        ERROR, errcode(sqlstate), errmsg("%s%s%s", pgch_msg_prefix, what ? what : "", m)
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
    [CHC_UUID]         = UUIDOID,
    [CHC_IPV4]         = INETOID,
    [CHC_IPV6]         = INETOID,
};

const int64_t pgch_pow10[10] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
};

Oid
pgch_datum_oid(const chc_type* type) {
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
        return pgch_datum_oid(chc_type_child(type, 0));
    case CHC_ARRAY:
        return ANYARRAYOID;
    case CHC_TUPLE:
        return RECORDOID;
    default:
        pgch_errorf(
            ERRCODE_FDW_INVALID_DATA_TYPE,
            "unsupported column type \"%s\"",
            chc_type_name(type, NULL)
        );
    }
    pg_unreachable();
}

Oid
pgch_native_oid(const chc_type* type) {
    chc_kind kind = chc_type_kind(type);
    Oid oid       = pgch_kind_oids[kind];

    if (OidIsValid(oid)) {
        return oid;
    }

    switch (kind) {
    case CHC_NULLABLE:
    case CHC_LOW_CARDINALITY:
        return pgch_native_oid(chc_type_child(type, 0));
    case CHC_ARRAY: {
        /*
         * PG has one array type per element type regardless of nesting, so
         * walk past nested Array layers before looking it up.
         */
        const chc_type* leaf = type;
        Oid array_type;

        while (chc_type_kind(leaf) == CHC_ARRAY) {
            leaf = chc_type_child(leaf, 0);
        }
        array_type = get_array_type(pgch_native_oid(leaf));
        if (!OidIsValid(array_type)) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE,
                "no PG array type for \"%s\"",
                chc_type_name(leaf, NULL)
            );
        }
        return array_type;
    }
    default:
        return pgch_datum_oid(type);
    }
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
        /* Nullable lives inside LowCardinality, not above it. */
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

/* ---- byte buffer ---------------------------------------------------- */

void
pgch_buf_reserve(pgch_buf* b, size_t need) {
    if (need <= b->cap) {
        return;
    }
    size_t ncap = b->cap ? b->cap : 64;

    while (ncap < need) {
        ncap *= 2;
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
