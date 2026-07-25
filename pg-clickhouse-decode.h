/*
 * pg-clickhouse-decode.h -- Native block -> PostgreSQL Datum.
 *
 * Walks clickhouse-c's wire-shaped column accessors and builds Datums a row
 * at a time. Per-row transforms (decimal to numeric, IPv4/IPv6 to inet, UUID
 * byteswap, enum name lookup, LowCardinality key deref, Nullable strip) happen
 * inline at read time. Array and Tuple columns land as pgch_array / pgch_tuple
 * carriers; pgch_convert turns those into real PG arrays and records once the
 * target type is known.
 *
 * Exactly one TU must `#define PGCH_IMPLEMENTATION` before including; other
 * TUs include for declarations only. Depends on pg-clickhouse.h.
 */

#ifndef PG_CLICKHOUSE_DECODE_H
#define PG_CLICKHOUSE_DECODE_H

#include "pg-clickhouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read one value. *valtype arrives holding the caller's preferred OID and
 * leaves holding the OID of the returned Datum; only CHC_JSON (JSONOID keeps
 * CH's verbatim text out of a jsonb round trip) and, from PG 19, CHC_UINT64
 * (OID8OID takes the range above 2^63 - 1) honor the incoming value, every
 * other kind overwrites it. Raises on unsupported types.
 */
extern Datum
pgch_read_value(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
);

/*
 * Type-tree signature over mapped PG OIDs. Two types with equal shapes
 * produce interchangeable Datums, so cached array layouts and tuple
 * descriptors stay valid across blocks.
 */
extern char*
pgch_type_shape(const chc_type* type);

/*
 * Block supply. next_block hands ownership to the reader, which destroys it
 * with pgch_alloc. Return NULL at end of stream, or on failure with error()
 * reporting the cause. error() is also polled before the first block.
 */
typedef struct pgch_block_source {
    void* ud;
    const chc_block* (*next_block)(void* ud);
    const char* (*error)(void* ud);
} pgch_block_source;

/*
 * Byte supply, for a caller holding chunks rather than blocks: the chc_io
 * that copies out of the current chunk and refills on drain, plus the
 * chc_in over it, are this header's rather than each consumer's.
 *
 * Defined only where the PGCH_IMPLEMENTATION TU also carries
 * CHC_IMPLEMENTATION, since it allocates a chc_in.
 */
typedef struct pgch_chunk_source {
    void* ud;
    /* Next chunk of Native bytes. *n == 0 is end of stream. Bytes must stay
       valid until the following call. Return false having set *error to fail. */
    bool (*next_chunk)(void* ud, const void** p, size_t* n, char** error);
    bool (*cancelled)(void* ud); /* optional, polled between refills */
} pgch_chunk_source;

/*
 * Row cursor over a block stream. values / nulls / coltypes are ncols long
 * and refreshed by each pgch_reader_next; values point into the current
 * block or into the context that was current at init, so consume a row
 * before asking for the next one.
 */
typedef struct pgch_reader {
    pgch_block_source src;

    Oid* coltypes;    /* pgch_datum_oid per column, caller may override
                         JSONB->JSON and, from PG 19, bigint->oid8 */
    char** colshapes; /* first-block shapes, for the stability check */
    Datum* values;
    bool* nulls;

    size_t ncols;
    size_t row;           /* next row in cur */
    const chc_block* cur; /* owned; destroyed on load of the next block */
    MemoryContext cxt;    /* context current at init; holds error and shapes */
    char* error;
    bool done;
} pgch_reader;

/*
 * Load the first block and map its schema. Zero columns, an immediate error
 * or an empty stream all leave the reader done; check reader->error. The
 * reader records CurrentMemoryContext and allocates its own state there.
 */
extern void
pgch_reader_init(pgch_reader* r, const pgch_block_source* src);

/*
 * As pgch_reader_init over a chunk source, assembling blocks across chunk
 * boundaries. NULL opts means pgch_block_opts_local. A chunk source drained
 * mid-block ends the stream with a truncation error rather than cleanly.
 */
extern void
pgch_reader_init_chunks(
    pgch_reader* r,
    const pgch_chunk_source* src,
    const chc_block_opts* opts
);

/*
 * Fill values / nulls with the next row. False at end of stream or on error,
 * which is reported in reader->error. Advances blocks as needed and rejects a
 * block whose schema drifted from the first one.
 */
extern bool
pgch_reader_next(pgch_reader* r);

extern size_t
pgch_reader_columns(const pgch_reader* r);

/* Destroy the current block. error stays until its context goes away. */
extern void
pgch_reader_free(pgch_reader* r);

/*
 * Conversion from the Datum pgch_read_value produced (intype, per
 * pgch_reader.coltypes) to the type the caller wants (outtype). Handles the
 * pgch_array / pgch_tuple carriers, text input functions, and CAST pathways.
 * val must be a representative value: array and tuple state is built from its
 * shape. Returns NULL when no conversion is needed, in which case pass the
 * Datum through untouched. State is allocated in CurrentMemoryContext, so
 * build it somewhere that outlives the per-row loop.
 */
extern void*
pgch_convert_init(Datum val, Oid intype, Oid outtype);

/*
 * The same state built from the column's CH type instead of a value, so a
 * column that starts with a NULL row, or holds nothing but NULLs, needs no
 * lazy per-column initialization. pgch_reader_convert_init reads the type off
 * the reader's current block and honors a coltypes override.
 */
extern void*
pgch_convert_init_type(const chc_type* in, Oid outtype);

extern void*
pgch_reader_convert_init(const pgch_reader* r, size_t col, Oid outtype);

extern Datum
pgch_convert(void* state, Datum val);

extern void
pgch_convert_free(void* state);

/*
 * The current row through per-column conversion states into values / nulls,
 * both ncols long. states may be NULL, and so may any of its entries, which
 * passes that column's Datum through.
 */
extern void
pgch_reader_fill(const pgch_reader* r, void** states, Datum* values, bool* nulls);

/*
 * Render a decoded value as a palloc'd cstring. Arrays and tuples route
 * through pgch_convert first, since their carriers have no output function.
 */
extern char*
pgch_value_to_cstring(Oid coltype, Datum value);

#ifdef PGCH_IMPLEMENTATION

#include <string.h>
#include <sys/socket.h> /* AF_INET, expanded by PG inet macros */

#include "access/htup_details.h"
#include "access/tupconvert.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "port/pg_bswap.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/uuid.h"
#if PG_VERSION_NUM >= 190000
#include "varatt.h"
#endif

/* ---- little-endian fixed-width reads at row offset ------------------- */

static inline int8_t
pgch__rd_i8(const uint8_t* p, uint64_t row) {
    return (int8_t)p[row];
}

static inline uint8_t
pgch__rd_u8(const uint8_t* p, uint64_t row) {
    return p[row];
}

static inline bool
pgch__rd_bool(const bool* p, uint64_t row) {
    return (bool)p[row];
}

#define PGCH__RD_FIXED(suffix, T)                                                      \
    static inline T pgch__rd_##suffix(const uint8_t* p, uint64_t row) {                \
        T v;                                                                           \
        memcpy(&v, p + row * sizeof(T), sizeof(T));                                    \
        return v;                                                                      \
    }

PGCH__RD_FIXED(i16, int16_t)
PGCH__RD_FIXED(u16, uint16_t)
PGCH__RD_FIXED(i32, int32_t)
PGCH__RD_FIXED(u32, uint32_t)
PGCH__RD_FIXED(i64, int64_t)
PGCH__RD_FIXED(f32, float)
PGCH__RD_FIXED(f64, double)
PGCH__RD_FIXED(u64, uint64_t)

static inline void
pgch__slice_str(
    const chc_column* col,
    uint64_t row,
    const char** out_ptr,
    size_t* out_len
) {
    const uint64_t* offs = chc_column_string_offsets(col);
    const uint8_t* data  = chc_column_string_data(col);
    uint64_t start       = row == 0 ? 0 : offs[row - 1];
    uint64_t end         = offs[row];

    *out_ptr = (const char*)data + start;
    *out_len = (size_t)(end - start);
}

/* ---- per-kind readers ----------------------------------------------- */

/*
 * Format a ClickHouse Decimal (two's-complement signed integer in LE bytes of
 * width 4/8/16/32 for Decimal32/64/128/256, with `scale` fractional digits
 * carried on the column type) into `out`. Returns bytes written, -1 on overflow.
 */
static int
pgch__format_decimal(
    const uint8_t* bytes,
    size_t width,
    uint32_t scale,
    char* out,
    size_t out_cap
) {
    uint32_t mag[8];
    char buf[80];
    size_t nwords = width / 4;
    bool neg      = false;

    if (width == 0 || width > 32 || width % 4 != 0 || scale >= sizeof(buf)) {
        return -1;
    }
    memcpy(mag, bytes, width);
    /* top bit of MSW is sign; negate two's-complement to get magnitude */
    if (mag[nwords - 1] & 0x80000000u) {
        neg = true;
        for (size_t i = 0; i < nwords; i++) {
            mag[i] = ~mag[i];
        }
        uint64_t carry = 1;

        for (size_t i = 0; i < nwords && carry; i++) {
            uint64_t v = (uint64_t)mag[i] + carry;

            mag[i] = (uint32_t)v;
            carry  = v >> 32;
        }
    }

    int n = 0;
    bool nonzero;

    /* base-10 division of mag yields digits LSB-first */
    do {
        uint64_t rem = 0;

        nonzero = false;
        for (ssize_t i = (ssize_t)nwords - 1; i >= 0; i--) {
            uint64_t v = (rem << 32) | mag[i];

            mag[i] = (uint32_t)(v / 10);
            rem    = v % 10;
            if (mag[i]) {
                nonzero = true;
            }
        }
        buf[n++] = (char)('0' + (uint32_t)rem);
    } while (nonzero && n < (int)sizeof(buf));

    /* pad leading zeros so digit count covers fractional portion */
    while (n <= (int)scale) {
        buf[n++] = '0';
    }

    size_t need = (size_t)neg + (size_t)n + (scale ? 1 : 0) + 1;

    if (need > out_cap) {
        return -1;
    }
    char* p = out;

    if (neg) {
        *p++ = '-';
    }

    /* emit MSD-first, inserting '.' before `scale` trailing digits */
    for (int i = n - 1; i >= 0; i--) {
        if (i + 1 == (int)scale) {
            *p++ = '.';
        }
        *p++ = buf[i];
    }
    *p = '\0';
    return (int)(p - out);
}

static Datum
pgch__read_decimal(const chc_column* col, const chc_type* type, uint64_t row) {
    size_t es;
    const uint8_t* p = (const uint8_t*)chc_column_fixed_data(col, &es);
    uint32_t scale   = (uint32_t)chc_type_decimal_scale(type);
    char buf[80];
    int rc;

#if PG_VERSION_NUM >= 140000
    /* Decimal32/64 fit in int64; skip the byte-array text path. */
    if (es == 4) {
        return NumericGetDatum(
            int64_div_fast_to_numeric(pgch__rd_i32(p, row), (int)scale)
        );
    }
    if (es == 8) {
        return NumericGetDatum(
            int64_div_fast_to_numeric(pgch__rd_i64(p, row), (int)scale)
        );
    }
#endif

    rc = pgch__format_decimal(p + row * es, es, scale, buf, sizeof(buf));
    if (rc < 0) {
        pgch_error(ERRCODE_FDW_ERROR, "decimal too wide");
    }
    return DirectFunctionCall3(
        numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(0), Int32GetDatum(-1)
    );
}

static Datum
pgch__read_string(const chc_column* col, uint64_t row) {
    const char* p;
    size_t len;

    pgch__slice_str(col, row, &p, &len);
    return PointerGetDatum(cstring_to_text_with_len(p, len));
}

static Datum
pgch__read_fixedstring(const chc_column* col, uint64_t row) {
    size_t width;
    const uint8_t* base = chc_column_fixed_data(col, &width);

    return PointerGetDatum(
        cstring_to_text_with_len((const char*)base + row * width, width)
    );
}

static Datum
pgch__read_uuid(const chc_column* col, uint64_t row) {
    pg_uuid_t* u     = (pg_uuid_t*)palloc(sizeof(pg_uuid_t));
    const uint8_t* p = (uint8_t*)chc_column_fixed_data(col, NULL) + row * 16;
    uint64_t a, b;

    memcpy(&a, p, 8);
    memcpy(&b, p + 8, 8);
    a = pg_hton64(a);
    b = pg_hton64(b);
    memcpy(u->data, &a, 8);
    memcpy(u->data + 8, &b, 8);
    return UUIDPGetDatum(u);
}

/*
 * IPv4 wire format: native uint32 (LE on supported hosts). PG inet wants
 * BE bytes of dotted-quad, so pg_hton32 the value into ip_addr directly.
 */
static Datum
pgch__read_ipv4(const chc_column* col, uint64_t row) {
    inet* res = (inet*)palloc0(sizeof(inet));
    uint32_t addr;

    memcpy(&addr, (const uint8_t*)chc_column_fixed_data(col, NULL) + row * 4, 4);
    addr           = pg_hton32(addr);
    ip_family(res) = PGSQL_AF_INET;
    ip_bits(res)   = 32;
    memcpy(ip_addr(res), &addr, 4);
    SET_INET_VARSIZE(res);
    return InetPGetDatum(res);
}

/* IPv6 wire is already network order; same layout as PG inet ip_addr. */
static Datum
pgch__read_ipv6(const chc_column* col, uint64_t row) {
    inet* res = (inet*)palloc0(sizeof(inet));

    ip_family(res) = PGSQL_AF_INET6;
    ip_bits(res)   = 128;
    memcpy(
        ip_addr(res), (const uint8_t*)chc_column_fixed_data(col, NULL) + row * 16, 16
    );
    SET_INET_VARSIZE(res);
    return InetPGetDatum(res);
}

static Datum
pgch__read_enum(const chc_column* col, const chc_type* type, uint64_t row) {
    size_t es;
    const uint8_t* p = (const uint8_t*)chc_column_fixed_data(col, &es);
    int64_t v        = 0;

    if (es == 1) {
        v = (int8_t)p[row];
    } else {
        int16_t t;

        memcpy(&t, p + row * 2, 2);
        v = t;
    }

    size_t n = chc_type_enum_count(type);

    for (size_t i = 0; i < n; i++) {
        const char* en;
        size_t el;
        int64_t ev;

        chc_type_enum_at(type, i, &en, &el, &ev);
        if (ev == v) {
            return PointerGetDatum(cstring_to_text_with_len(en ? en : "", el));
        }
    }
    return PointerGetDatum(cstring_to_text_with_len("", 0));
}

/*
 * JSON body bytes are STRING-serialized document text (the server needs
 * output_format_native_write_json_as_string=1 to emit that). Run them through
 * json_in / jsonb_in depending on the caller's target valtype.
 */
static Datum
pgch__read_json(const chc_column* col, uint64_t row, Oid valtype) {
    const char* p;
    size_t len;
    char* cstr;
    Datum ret;

    pgch__slice_str(col, row, &p, &len);
    cstr = palloc(len + 1);
    memcpy(cstr, p, len);
    cstr[len] = '\0';
    ret       = DirectFunctionCall1(
        valtype == JSONOID ? json_in : jsonb_in, CStringGetDatum(cstr)
    );
    pfree(cstr);
    return ret;
}

/*
 * LowCardinality(String) or LowCardinality(Nullable(String)). The row's key
 * indexes the dict column; the dict's first slot is the null sentinel for the
 * Nullable variant.
 */
static Datum
pgch__read_lc(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    int ks            = chc_column_lc_key_size(col);
    const uint8_t* kp = (const uint8_t*)chc_column_lc_keys(col) + (size_t)row * ks;
    uint64_t k        = 0;

    switch (ks) {
    case 1:
        k = kp[0];
        break;
    case 2: {
        uint16_t v;

        memcpy(&v, kp, 2);
        k = v;
        break;
    }
    case 4: {
        uint32_t v;

        memcpy(&v, kp, 4);
        k = v;
        break;
    }
    case 8:
        memcpy(&k, kp, 8);
        break;
    default:
        pgch_errorf(ERRCODE_FDW_ERROR, "unexpected LowCardinality key size %d", ks);
    }

    const chc_column* dict  = chc_column_lc_dict(col);
    const chc_type* inner_t = chc_type_child(type, 0);

    if (chc_type_kind(inner_t) == CHC_NULLABLE &&
        chc_column_layout(dict) == CHC_COL_NULLABLE) {
        const uint8_t* dnm = chc_column_null_map(dict);

        if (dnm && dnm[k]) {
            *valtype = TEXTOID;
            *is_null = true;
            return (Datum)0;
        }
        dict = chc_column_nullable_inner(dict);
    }

    *valtype = TEXTOID;
    *is_null = false;
    if (chc_column_layout(dict) != CHC_COL_STRING) {
        pgch_error(
            ERRCODE_FDW_INVALID_DATA_TYPE, "unsupported LowCardinality inner type"
        );
    }
    return pgch__read_string(dict, k);
}

static Datum
pgch__read_array(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    const uint64_t* offs    = chc_column_array_offsets(col);
    uint64_t start          = row == 0 ? 0 : offs[row - 1];
    uint64_t end            = offs[row];
    uint64_t len            = end - start;
    const chc_type* inner_t = chc_type_child(type, 0);
    const chc_column* inner = chc_column_array_values(col);
    pgch_array* slot        = (pgch_array*)palloc(sizeof(pgch_array));
    const chc_type* leaf    = type;
    int ndim                = 0;

    /*
     * PG has one array type per element type regardless of nesting, so walk
     * past nested Array layers to the leaf scalar type.
     */
    while (chc_type_kind(leaf) == CHC_ARRAY) {
        ndim++;
        leaf = chc_type_child(leaf, 0);
    }

    slot->len        = len;
    slot->ndim       = ndim;
    slot->item_type  = pgch_datum_oid(leaf);
    slot->array_type = get_array_type(slot->item_type);
    if (!OidIsValid(slot->array_type)) {
        pgch_errorf(
            ERRCODE_FDW_INVALID_DATA_TYPE,
            "no PG array type for column type \"%s\"",
            chc_type_name(leaf, NULL)
        );
    }

    if (len > 0) {
        Oid scratch = slot->item_type;

        slot->datums = (Datum*)palloc0(sizeof(Datum) * len);
        slot->nulls  = (bool*)palloc0(sizeof(bool) * len);

        /*
         * For ndim == 1 pgch_read_value returns leaf scalars; for ndim > 1
         * inner_t is itself CHC_ARRAY so recursion produces nested
         * pgch_array. Use a scratch valtype to avoid clobbering item_type.
         */
        for (uint64_t i = 0; i < len; ++i) {
            slot->datums[i] =
                pgch_read_value(inner, inner_t, start + i, &scratch, &slot->nulls[i]);
        }
    } else {
        slot->datums = NULL;
        slot->nulls  = NULL;
    }

    *valtype = ANYARRAYOID;
    *is_null = false;
    return PointerGetDatum(slot);
}

static Datum
pgch__read_tuple(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    size_t n = chc_type_n_children(type);
    pgch_tuple* slot;

    if (n == 0) {
        pgch_error(ERRCODE_FDW_ERROR, "returned tuple is empty");
    }

    slot               = (pgch_tuple*)palloc(sizeof(pgch_tuple));
    slot->datums       = (Datum*)palloc(sizeof(Datum) * n);
    slot->nulls        = (bool*)palloc0(sizeof(bool) * n);
    slot->types        = (Oid*)palloc0(sizeof(Oid) * n);
    slot->len          = n;
    slot->ch_type_name = chc_type_name(type, NULL);

    for (size_t i = 0; i < n; ++i) {
        const chc_type* ft   = chc_type_child(type, i);
        const chc_column* fc = chc_column_tuple_child(col, i);

        slot->datums[i] =
            pgch_read_value(fc, ft, row, &slot->types[i], &slot->nulls[i]);
    }

    *valtype = RECORDOID;
    *is_null = false;
    return PointerGetDatum(slot);
}

Datum
pgch_read_value(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    /* Unwrap outer Nullable, handling nulls here. */
    if (chc_type_kind(type) == CHC_NULLABLE) {
        const chc_type* inner_t = chc_type_child(type, 0);

        if (chc_column_layout(col) == CHC_COL_NULLABLE) {
            const uint8_t* nm = chc_column_null_map(col);

            if (nm && nm[row]) {
                *valtype = pgch_datum_oid(inner_t);
                *is_null = true;
                return (Datum)0;
            }
            col = chc_column_nullable_inner(col);
        }
        type = inner_t;
    }
    *is_null = false;

    switch (chc_type_kind(type)) {
    case CHC_VOID:
    case CHC_NOTHING:
        *valtype = InvalidOid;
        *is_null = true;
        return (Datum)0;
    case CHC_UINT8:
        *valtype = INT2OID;
        return (Datum)pgch__rd_u8(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_BOOL:
        *valtype = BOOLOID;
        return (Datum)pgch__rd_bool((const bool*)chc_column_fixed_data(col, NULL), row);
    case CHC_INT8:
        *valtype = INT2OID;
        return (Datum)pgch__rd_i8(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_INT16:
        *valtype = INT2OID;
        return (Datum)pgch__rd_i16(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_UINT16:
        *valtype = INT4OID;
        return (Datum)pgch__rd_u16(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_INT32:
        *valtype = INT4OID;
        return (Datum)pgch__rd_i32(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_UINT32:
        *valtype = INT8OID;
        return Int64GetDatum(
            (int64)pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT64:
        *valtype = INT8OID;
        return Int64GetDatum(
            pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_UINT64: {
        uint64_t v =
            pgch__rd_u64((const uint8_t*)chc_column_fixed_data(col, NULL), row);

#if PG_VERSION_NUM >= 190000
        /*
         * oid8 spans the whole unsigned range, so a caller that pins it takes
         * values bigint cannot hold. Same in-out valtype as CHC_JSON.
         */
        if (*valtype == OID8OID) {
            return ObjectId8GetDatum(v);
        }
#endif
        if (v > (uint64_t)PG_INT64_MAX) {
            pgch_errorf(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "value " UINT64_FORMAT " is out of range of bigint",
                v
            );
        }
        *valtype = INT8OID;
        return Int64GetDatum((int64)v);
    }
    case CHC_FLOAT32:
        *valtype = FLOAT4OID;
        return Float4GetDatum(
            pgch__rd_f32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_FLOAT64:
        *valtype = FLOAT8OID;
        return Float8GetDatum(
            pgch__rd_f64((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_DECIMAL32:
    case CHC_DECIMAL64:
    case CHC_DECIMAL128:
    case CHC_DECIMAL256:
        *valtype = NUMERICOID;
        return pgch__read_decimal(col, type, row);
    case CHC_STRING:
        *valtype = TEXTOID;
        return pgch__read_string(col, row);
    case CHC_ENUM8:
    case CHC_ENUM16:
        *valtype = TEXTOID;
        return pgch__read_enum(col, type, row);
    case CHC_JSON:
    case CHC_OBJECT: {
        /*
         * *valtype arrives set to JSONBOID by default; honor a caller that
         * asked for JSONOID so CH's verbatim formatting survives.
         */
        Oid target = (*valtype == JSONOID) ? JSONOID : JSONBOID;

        *valtype = target;
        return pgch__read_json(col, row, target);
    }
    case CHC_FIXED_STRING:
        *valtype = TEXTOID;
        return pgch__read_fixedstring(col, row);
    case CHC_DATE:
        *valtype = DATEOID;
        return DateADTGetDatum(
            (DateADT)
                pgch__rd_u16((const uint8_t*)chc_column_fixed_data(col, NULL), row) -
            PGCH__DATE_OFFSET
        );
    case CHC_DATE32:
        *valtype = DATEOID;
        return DateADTGetDatum(
            (DateADT)
                pgch__rd_i32((const uint8_t*)chc_column_fixed_data(col, NULL), row) -
            PGCH__DATE_OFFSET
        );
    case CHC_DATETIME: {
        uint32_t secs =
            pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row);

        *valtype = TIMESTAMPTZOID;
        return TimestampTzGetDatum(time_t_to_timestamptz((pg_time_t)secs));
    }
    case CHC_DATETIME64: {
        int64 raw = pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row);
        uint32_t scale = chc_type_datetime64_scale(type);

        if (scale >= lengthof(pgch_pow10)) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE, "DateTime64 scale %u out of range", scale
            );
        }
        int64 power = pgch_pow10[scale];

        *valtype = TIMESTAMPTZOID;
        /* multiply before divide so scale > 6 keeps sub-second us */
        return TimestampTzGetDatum(
            time_t_to_timestamptz(raw / power) + (raw % power) * USECS_PER_SEC / power
        );
    }
    case CHC_TIME: {
        const uint8_t* p = (const uint8_t*)chc_column_fixed_data(col, NULL);

        *valtype = TIMEOID;
        return TimeADTGetDatum((TimeADT)pgch__rd_i32(p, row) * USECS_PER_SEC);
    }
    case CHC_TIME64: {
        int64 raw = pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row);
        uint32_t scale = chc_type_datetime64_scale(type);

        if (scale >= lengthof(pgch_pow10)) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE, "Time64 scale %u out of range", scale
            );
        }
        int64 power = pgch_pow10[scale];

        *valtype = TIMEOID;
        /* Split before scaling: scale 9 of a full day overflows int64. */
        return TimeADTGetDatum(
            (raw / power) * USECS_PER_SEC + (raw % power) * USECS_PER_SEC / power
        );
    }
    case CHC_UUID:
        *valtype = UUIDOID;
        return pgch__read_uuid(col, row);
    case CHC_IPV4:
        *valtype = INETOID;
        return pgch__read_ipv4(col, row);
    case CHC_IPV6:
        *valtype = INETOID;
        return pgch__read_ipv6(col, row);
    case CHC_LOW_CARDINALITY:
        return pgch__read_lc(col, type, row, valtype, is_null);
    case CHC_ARRAY:
        return pgch__read_array(col, type, row, valtype, is_null);
    case CHC_TUPLE:
        return pgch__read_tuple(col, type, row, valtype, is_null);
    default:
        pgch_errorf(
            ERRCODE_FDW_INVALID_DATA_TYPE,
            "unsupported type \"%s\" in Native format",
            chc_type_name(type, NULL)
        );
    }
    pg_unreachable();
}

/* ---- reader --------------------------------------------------------- */

static void
pgch__append_shape(StringInfo buf, const chc_type* type) {
    chc_kind kind = chc_type_kind(type);

    switch (kind) {
    case CHC_NULLABLE:
    case CHC_LOW_CARDINALITY:
        pgch__append_shape(buf, chc_type_child(type, 0));
        return;
    case CHC_ARRAY:
        appendStringInfoChar(buf, 'a');
        pgch__append_shape(buf, chc_type_child(type, 0));
        return;
    case CHC_TUPLE: {
        size_t n = chc_type_n_children(type);

        appendStringInfo(buf, "t%zu(", n);
        for (size_t i = 0; i < n; i++) {
            pgch__append_shape(buf, chc_type_child(type, i));
        }
        appendStringInfoChar(buf, ')');
        return;
    }
    default:
        appendStringInfo(buf, "%u;", pgch_kind_oids[kind]);
    }
}

char*
pgch_type_shape(const chc_type* type) {
    StringInfoData buf;

    initStringInfo(&buf);
    pgch__append_shape(&buf, type);
    return buf.data;
}

/*
 * Verify block schema matches the first block. Only guards against drift that
 * would corrupt Datum interpretation; the peer controls values regardless.
 */
static bool
pgch__check_schema(pgch_reader* r) {
    size_t ncols = chc_block_n_columns(r->cur);

    /* Errors go in r->cxt: the row loop's context may be reset under us. */
    if (ncols != r->ncols) {
        MemoryContext old = MemoryContextSwitchTo(r->cxt);

        r->error =
            psprintf("block column count changed from %zu to %zu", r->ncols, ncols);
        MemoryContextSwitchTo(old);
        return false;
    }

    for (size_t i = 0; i < ncols; i++) {
        char* shape = pgch_type_shape(chc_block_column_type(r->cur, i));
        bool match  = strcmp(shape, r->colshapes[i]) == 0;

        pfree(shape);
        if (!match) {
            MemoryContext old = MemoryContextSwitchTo(r->cxt);

            r->error = psprintf("block column %zu type changed", i + 1);
            MemoryContextSwitchTo(old);
            return false;
        }
    }
    return true;
}

static bool
pgch__load_block(pgch_reader* r) {
    if (r->cur) {
        chc_block_destroy(unconstify(chc_block*, r->cur), &pgch_alloc);
        r->cur = NULL;
    }
    r->cur = r->src.next_block(r->src.ud);
    if (r->cur == NULL) {
        const char* src_err = r->src.error(r->src.ud);

        if (src_err) {
            r->error = MemoryContextStrdup(r->cxt, src_err);
        }
        r->done = true;
        return false;
    }
    if (r->colshapes && !pgch__check_schema(r)) {
        chc_block_destroy(unconstify(chc_block*, r->cur), &pgch_alloc);
        r->cur  = NULL;
        r->done = true;
        return false;
    }
    return true;
}

size_t
pgch_reader_columns(const pgch_reader* r) {
    return r->ncols;
}

#ifdef CHC_IMPLEMENTATION

/* ---- chunk source --------------------------------------------------- */

typedef struct pgch__chunks {
    pgch_chunk_source src;
    chc_block_opts opts;
    chc_in* in;
    chc_io io;
    MemoryContext cxt;

    const uint8_t* cur; /* borrowed until the next next_chunk call */
    size_t len;
    size_t pos;
    char* error;
    bool eos;
} pgch__chunks;

/*
 * Copy out of the current chunk, refilling on drain. *out_n == 0 is what
 * clickhouse-c reads as end of stream: at a block boundary chc_block_read
 * reports a clean end, mid-block a short read.
 */
static int
pgch__chunk_read(void* ud, void* buf, size_t len, size_t* out_n, chc_err* err) {
    pgch__chunks* c = (pgch__chunks*)ud;

    while (c->pos == c->len) {
        const void* p = NULL;
        size_t n      = 0;
        char* e       = NULL;

        if (c->eos) {
            *out_n = 0;
            return CHC_OK;
        }
        if (!c->src.next_chunk(c->src.ud, &p, &n, &e)) {
            c->eos   = true;
            c->error = MemoryContextStrdup(c->cxt, e ? e : "chunk source failed");
            if (err) {
                snprintf(err->msg, sizeof(err->msg), "%s", c->error);
            }
            return CHC_ERR_IO;
        }
        if (n == 0) {
            c->eos = true;
            *out_n = 0;
            return CHC_OK;
        }
        c->cur = (const uint8_t*)p;
        c->len = n;
        c->pos = 0;
    }

    *out_n = Min(len, c->len - c->pos);
    memcpy(buf, c->cur + c->pos, *out_n);
    c->pos += *out_n;
    return CHC_OK;
}

static int
pgch__chunk_cancel(void* ud) {
    pgch__chunks* c = (pgch__chunks*)ud;

    return c->src.cancelled(c->src.ud) ? 1 : 0;
}

static const chc_block*
pgch__chunk_next_block(void* ud) {
    pgch__chunks* c = (pgch__chunks*)ud;
    chc_block* b    = NULL;
    chc_err err     = {};

    if (c->error) {
        return NULL;
    }
    if (chc_block_read(c->in, &pgch_alloc, &c->opts, &b, &err) != CHC_OK) {
        c->error = MemoryContextStrdup(
            c->cxt, err.msg[0] ? err.msg : "block read failed"
        );
        return NULL;
    }
    return b; /* NULL at clean end of stream */
}

static const char*
pgch__chunk_error(void* ud) {
    return ((pgch__chunks*)ud)->error;
}

void
pgch_reader_init_chunks(
    pgch_reader* r,
    const pgch_chunk_source* src,
    const chc_block_opts* opts
) {
    pgch__chunks* c = palloc0(sizeof(*c));
    pgch_block_source bsrc;
    chc_err err = {};

    c->src  = *src;
    c->opts = opts ? *opts : pgch_block_opts_local;
    c->cxt  = CurrentMemoryContext;
    c->io   = (chc_io){ .ud           = c,
                        .read         = pgch__chunk_read,
                        .check_cancel = src->cancelled ? pgch__chunk_cancel : NULL };
    c->in   = pgch_in_alloc();
    if (chc_in_init(c->in, &c->io, &pgch_alloc, 0, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "reader init: ");
    }

    bsrc.ud         = c;
    bsrc.next_block = pgch__chunk_next_block;
    bsrc.error      = pgch__chunk_error;
    pgch_reader_init(r, &bsrc);
}

#endif /* CHC_IMPLEMENTATION */

void
pgch_reader_init(pgch_reader* r, const pgch_block_source* src) {
    const char* src_err;
    size_t ncols;

    memset(r, 0, sizeof(*r));
    r->src = *src;
    r->cxt = CurrentMemoryContext;

    src_err = r->src.error(r->src.ud);
    if (src_err) {
        r->done  = true;
        r->error = pstrdup(src_err);
        return;
    }

    /* First block defines the schema; it may carry zero rows. */
    if (!pgch__load_block(r)) {
        return;
    }

    ncols    = chc_block_n_columns(r->cur);
    r->ncols = ncols;
    if (ncols == 0) {
        r->done = true;
        return;
    }

    r->coltypes  = palloc0(sizeof(Oid) * ncols);
    r->colshapes = palloc0(sizeof(char*) * ncols);
    r->values    = palloc0(sizeof(Datum) * ncols);
    r->nulls     = palloc0(sizeof(bool) * ncols);

    for (size_t i = 0; i < ncols; i++) {
        const chc_type* ct = chc_block_column_type(r->cur, i);

        r->coltypes[i]  = pgch_datum_oid(ct);
        r->colshapes[i] = pgch_type_shape(ct);
    }
}

bool
pgch_reader_next(pgch_reader* r) {
    size_t ncols;

    if (r->done || r->coltypes == NULL || r->error) {
        return false;
    }

    ncols = r->ncols;

    /* cur is non-NULL here: init loads the first block, load replaces cur */
    while (r->row >= chc_block_n_rows(r->cur)) {
        r->row = 0;
        if (!pgch__load_block(r)) {
            return false;
        }
    }

    PG_TRY();
    {
        for (size_t i = 0; i < ncols; i++) {
            /*
             * coltypes[i] is passed in so callers can pin CHC_JSON to JSONOID
             * and keep CH's verbatim text, or CHC_UINT64 to OID8OID for its
             * top half; every other kind overwrites it.
             */
            Oid t                 = r->coltypes[i];
            const chc_column* col = chc_block_column(r->cur, i);
            const chc_type* ct    = chc_block_column_type(r->cur, i);

            r->values[i] = pgch_read_value(col, ct, r->row, &t, &r->nulls[i]);
        }
    }
    PG_CATCH();
    {
        MemoryContext oldcxt = MemoryContextSwitchTo(r->cxt);
        ErrorData* edata     = CopyErrorData();
        const char* msg      = edata->message ? edata->message : "unknown error";
        size_t plen          = sizeof(PGCH_MSG_PREFIX) - 1;

        /* Callers re-prefix when reporting; don't carry it twice. */
        if (plen && strncmp(msg, PGCH_MSG_PREFIX, plen) == 0) {
            msg += plen;
        }
        r->error = pstrdup(msg);
        FlushErrorState();
        FreeErrorData(edata);
        MemoryContextSwitchTo(oldcxt);
        r->done = true;
        return false;
    }
    PG_END_TRY();

    r->row++;
    return true;
}

void
pgch_reader_free(pgch_reader* r) {
    if (r->cur) {
        chc_block_destroy(unconstify(chc_block*, r->cur), &pgch_alloc);
        r->cur = NULL;
    }
    /* error is palloc'd in r->cxt; it goes away with that context. */
    r->error = NULL;
}

/* ---- conversion ----------------------------------------------------- */

typedef struct pgch_convert_state pgch_convert_state;
typedef Datum (*pgch__convert_fn)(pgch_convert_state*, Datum);

struct pgch_convert_state {
    Oid intype;
    Oid outtype;
    pgch__convert_fn func;

    /* record */
    TupleConversionMap* tupmap;
    TupleDesc indesc;
    TupleDesc outdesc;
    pgch_convert_state** field_states;

    /* array */
    Oid item_type; /* element type of the array built, post elem_state */
    pgch_convert_state* elem_state;
    int16 typlen;
    bool typbyval;
    char typalign;

    /* text */
    int32 typmod;
    Oid typinput;
    Oid typioparam;

    /* generic */
    CoercionPathType ctype;
    Oid castfunc;
};

static inline Datum
pgch__convert_generic(pgch_convert_state* state, Datum val) {
    if (state->ctype == COERCION_PATH_FUNC) {
        Assert(state->castfunc != InvalidOid);
        val = OidFunctionCall1(state->castfunc, val);
    }

    return val;
}

static Datum
pgch__convert_record(pgch_convert_state* state, Datum val) {
    HeapTuple temptup;
    HeapTuple htup;
    pgch_tuple* slot = (pgch_tuple*)DatumGetPointer(val);

    for (size_t i = 0; i < slot->len; i++) {
        pgch_convert_state* s = state->field_states[i];

        /* Null fields carry no value to convert or to build state from. */
        if (slot->nulls[i]) {
            continue;
        }

        if (s == NULL && slot->types[i] == RECORDOID) {
            MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(state));

            /* indesc carries the field's target type, composite or RECORDOID. */
            s = pgch_convert_init(
                slot->datums[i], RECORDOID, TupleDescAttr(state->indesc, i)->atttypid
            );
            MemoryContextSwitchTo(oldcxt);
            state->field_states[i] = s;
        }

        if (s) {
            slot->datums[i] = s->func(s, slot->datums[i]);
        }
    }

    htup = heap_form_tuple(state->indesc, slot->datums, slot->nulls);
    if (!state->outdesc) {
        val = heap_copy_tuple_as_datum(htup, state->indesc);

        if (state->outtype == TEXTOID) {
            /* a lot of allocations, not so efficient */
            val = CStringGetTextDatum(
                DatumGetCString(OidFunctionCall1(F_RECORD_OUT, val))
            );
        }
    } else {
        if (state->tupmap) {
            temptup = execute_attr_map_tuple(htup, state->tupmap);
        } else {
            temptup = htup;
        }

        val = heap_copy_tuple_as_datum(temptup, state->outdesc);
    }

    return val;
}

/*
 * Walk a nested pgch_array into a flat datum buffer, verifying each level
 * matches the dims taken from the first child. Returns false if the shape is
 * jagged so the caller can fall back to a slower path.
 */
static bool
pgch__flatten_array(
    pgch_array* slot,
    int* dims,
    int level,
    Datum* values,
    bool* nulls,
    size_t* idx
) {
    if ((int)slot->len != dims[level]) {
        return false;
    }

    if (slot->ndim == 1) {
        for (size_t i = 0; i < slot->len; i++) {
            values[*idx] = slot->datums[i];
            nulls[*idx]  = slot->nulls[i];
            (*idx)++;
        }
    } else {
        for (size_t i = 0; i < slot->len; i++) {
            pgch_array* child = (pgch_array*)DatumGetPointer(slot->datums[i]);

            if (!pgch__flatten_array(child, dims, level + 1, values, nulls, idx)) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Emit a nested pgch_array as a PG array text literal, quoting each leaf and
 * escaping `\` and `"`. Jagged fallback, so a ragged CH array surfaces
 * array_in's malformed-literal error rather than a wrong shape.
 */
static void
pgch__emit_array_text(pgch_array* slot, FmgrInfo* outfn, StringInfo buf) {
    appendStringInfoChar(buf, '{');
    for (size_t i = 0; i < slot->len; i++) {
        if (i > 0) {
            appendStringInfoChar(buf, ',');
        }

        if (slot->ndim > 1) {
            pgch_array* child = (pgch_array*)DatumGetPointer(slot->datums[i]);

            pgch__emit_array_text(child, outfn, buf);
        } else if (slot->nulls[i]) {
            appendStringInfoString(buf, "NULL");
        } else {
            char* s = OutputFunctionCall(outfn, slot->datums[i]);

            appendStringInfoChar(buf, '"');
            for (char* p = s; *p; p++) {
                if (*p == '"' || *p == '\\') {
                    appendStringInfoChar(buf, '\\');
                }
                appendStringInfoChar(buf, *p);
            }
            appendStringInfoChar(buf, '"');
            pfree(s);
        }
    }
    appendStringInfoChar(buf, '}');
}

/* Convert leaf elements in place; interior levels are carriers, not values. */
static void
pgch__convert_elems(pgch_convert_state* elem, pgch_array* slot) {
    for (size_t i = 0; i < slot->len; i++) {
        if (slot->ndim > 1) {
            pgch__convert_elems(elem, (pgch_array*)DatumGetPointer(slot->datums[i]));
        } else if (!slot->nulls[i]) {
            slot->datums[i] = elem->func(elem, slot->datums[i]);
        }
    }
}

static Datum
pgch__convert_array(pgch_convert_state* state, Datum val) {
    pgch_array* slot = (pgch_array*)DatumGetPointer(val);

    if (state->elem_state) {
        pgch__convert_elems(state->elem_state, slot);
    }

    if (slot->len == 0) {
        val = PointerGetDatum(construct_empty_array(state->item_type));
    } else if (slot->ndim <= 1) {
        int dims[1] = { (int)slot->len };
        int lbs[1]  = { 1 };

        val = PointerGetDatum(construct_md_array(
            slot->datums,
            slot->nulls,
            1,
            dims,
            lbs,
            state->item_type,
            state->typlen,
            state->typbyval,
            state->typalign
        ));
    } else {
        int dims[MAXDIM] = {};
        int lbs[MAXDIM]  = {};
        size_t idx       = 0;
        Datum* flat;
        bool* flatnulls;
        pgch_array* probe = slot;

        if (slot->ndim > MAXDIM) {
            pgch_errorf(
                ERRCODE_PROGRAM_LIMIT_EXCEEDED,
                "nested array depth %d exceeds maximum %d",
                slot->ndim,
                MAXDIM
            );
        }

        for (int d = 0; d < slot->ndim; d++) {
            dims[d] = (int)probe->len;
            lbs[d]  = 1;
            if (probe->ndim > 1 && probe->len > 0) {
                probe = (pgch_array*)DatumGetPointer(probe->datums[0]);
            }
        }

        size_t total = ArrayGetNItems(slot->ndim, dims);

        if (total == 0) {
            val = PointerGetDatum(construct_empty_array(state->item_type));
        } else {
            flat      = palloc(sizeof(Datum) * total);
            flatnulls = palloc0(sizeof(bool) * total);

            if (pgch__flatten_array(slot, dims, 0, flat, flatnulls, &idx)) {
                val = PointerGetDatum(construct_md_array(
                    flat,
                    flatnulls,
                    slot->ndim,
                    dims,
                    lbs,
                    state->item_type,
                    state->typlen,
                    state->typbyval,
                    state->typalign
                ));
            } else {
                StringInfoData buf;
                FmgrInfo outfn;
                Oid out_func;
                Oid in_func;
                Oid ioparam;
                bool varlena;

                pfree(flat);
                pfree(flatnulls);

                getTypeOutputInfo(state->item_type, &out_func, &varlena);
                fmgr_info(out_func, &outfn);

                initStringInfo(&buf);
                pgch__emit_array_text(slot, &outfn, &buf);

                getTypeInputInfo(state->intype, &in_func, &ioparam);
                val = OidInputFunctionCall(in_func, buf.data, ioparam, -1);

                pfree(buf.data);
            }
        }
    }

    return pgch__convert_generic(state, val);
}

static Datum
pgch__convert_from_text(pgch_convert_state* state, Datum val) {
    return OidInputFunctionCall(
        state->typinput, TextDatumGetCString(val), state->typioparam, state->typmod
    );
}

/* UInt8 decodes as int2 (CH's historical bool); narrow it back. */
static Datum
pgch__convert_bool(pgch_convert_state* state pg_attribute_unused(), Datum val) {
    return BoolGetDatum(DatumGetInt16(val));
}

Datum
pgch_convert(void* state, Datum val) {
    return state ? ((pgch_convert_state*)state)->func(state, val) : val;
}

/* First non-null leaf of a nested carrier, for building element state. */
static bool
pgch__array_leaf(const pgch_array* slot, Datum* out) {
    for (size_t i = 0; i < slot->len; i++) {
        if (slot->ndim > 1) {
            if (pgch__array_leaf((pgch_array*)DatumGetPointer(slot->datums[i]), out)) {
                return true;
            }
        } else if (!slot->nulls[i]) {
            *out = slot->datums[i];
            return true;
        }
    }
    return false;
}

/*
 * Shared by both entry points. `ct` is the column's CH type when the caller
 * had one, in which case val is not read: every shape the value would have
 * supplied comes off the type instead.
 */
static pgch_convert_state*
pgch__convert_init(const chc_type* ct, Datum val, Oid intype, Oid outtype) {
    /* Both are raw byte carriers; no cast needed. */
    if (intype == TEXTOID && outtype == BYTEAOID) {
        return NULL;
    }
    /* Nothing / Void columns are always NULL, so there is nothing to convert. */
    if (!OidIsValid(intype)) {
        return NULL;
    }

    pgch_convert_state* state = palloc0(sizeof(pgch_convert_state));

    if (ct) {
        ct = pgch_unwrap(ct, NULL);
    }
    state->intype  = intype;
    state->outtype = outtype;
    state->typmod  = -1;
    state->ctype   = COERCION_PATH_NONE;

    if (intype == ANYARRAYOID) {
        pgch_array* slot     = ct ? NULL : (pgch_array*)DatumGetPointer(val);
        const chc_type* leaf = ct;
        /* A domain over an array carries no typelem of its own. */
        Oid out_elem =
            OidIsValid(outtype) ? get_element_type(getBaseType(outtype)) : InvalidOid;

        if (ct) {
            while (chc_type_kind(leaf) == CHC_ARRAY) {
                leaf = chc_type_child(leaf, 0);
            }
            state->item_type = pgch_datum_oid(leaf);
            state->intype    = pgch_native_oid(ct);
        } else {
            state->item_type = slot->item_type;
            state->intype    = slot->array_type;
        }
        state->func = pgch__convert_array;

        /*
         * find_coercion_pathway answers ARRAYCOERCE for a pair of array types,
         * which has no runtime here, so element conversion is its own state
         * run over the carrier's leaves. The array is then built at the
         * target's element type directly.
         */
        if (OidIsValid(out_elem) && out_elem != state->item_type) {
            Datum leafval  = (Datum)0;
            bool have_leaf = ct || pgch__array_leaf(slot, &leafval);

            if (have_leaf || state->item_type != RECORDOID) {
                state->elem_state = pgch__convert_init(
                    leaf, leafval, state->item_type, out_elem
                );
                state->item_type = out_elem;
                state->intype    = outtype;
            }
        }
        get_typlenbyvalalign(
            state->item_type, &state->typlen, &state->typbyval, &state->typalign
        );
        intype = state->intype;
    }

    if (intype == RECORDOID) {
        pgch_tuple* slot = ct ? NULL : (pgch_tuple*)DatumGetPointer(val);
        size_t nfields   = ct ? chc_type_n_children(ct) : slot->len;

        state->func         = pgch__convert_record;
        state->indesc       = CreateTemplateTupleDesc(nfields);
        state->field_states = palloc(sizeof(void*) * nfields);

        /*
         * Resolve the target descriptor before the fields: a nested Tuple has
         * to convert into whatever composite the target declares for that
         * field, otherwise it lands as an anonymous record and the tuple map
         * rejects it on type.
         */
        if (!(outtype == RECORDOID || outtype == TEXTOID)) {
            TypeCacheEntry* typentry;
            TupleDesc tupdesc;

            typentry = lookup_type_cache(
                outtype, TYPECACHE_TUPDESC | TYPECACHE_DOMAIN_BASE_INFO
            );

            if (typentry->typtype == TYPTYPE_DOMAIN) {
                tupdesc = lookup_rowtype_tupdesc_noerror(
                    typentry->domainBaseType, typentry->domainBaseTypmod, false
                );
            } else {
                if (typentry->tupDesc == NULL) {
                    const char* tname =
                        ct ? chc_type_name(ct, NULL) : slot->ch_type_name;

                    pgch_errorf(
                        ERRCODE_WRONG_OBJECT_TYPE,
                        "cannot return %s as %s",
                        tname ? tname : "?",
                        format_type_be(outtype)
                    );
                }

                tupdesc = typentry->tupDesc;
                PinTupleDesc(tupdesc);
            }
            state->outdesc = CreateTupleDescCopy(tupdesc);
            ReleaseTupleDesc(tupdesc);
        }

        for (size_t i = 0; i < nfields; ++i) {
            const chc_type* ft = ct ? chc_type_child(ct, i) : NULL;
            Oid ftype          = ct ? pgch_datum_oid(ft) : slot->types[i];
            bool isnull        = ct ? false : slot->nulls[i];
            Oid item_type      = ftype;

            if (ftype == ANYARRAYOID && !isnull) {
                item_type =
                    ct ? pgch_native_oid(ft)
                       : ((pgch_array*)DatumGetPointer(slot->datums[i]))->array_type;
            }
            if (state->outdesc && ftype == RECORDOID &&
                i < (size_t)state->outdesc->natts) {
                item_type = TupleDescAttr(state->outdesc, i)->atttypid;
            }

            /* Null fields hold Datum 0; convert_record fills these lazily. */
            state->field_states[i] =
                isnull ? NULL
                       : pgch__convert_init(
                             ft, ct ? (Datum)0 : slot->datums[i], ftype, item_type
                         );

            TupleDescInitEntry(state->indesc, (AttrNumber)i + 1, "", item_type, -1, 0);
        }

#if PG_VERSION_NUM >= 190000
        TupleDescFinalize(state->indesc);
#endif
        state->indesc = BlessTupleDesc(state->indesc);

        if (state->outdesc) {
            state->tupmap = convert_tuples_by_position(
                state->indesc, state->outdesc, "could not map tuple to returned type"
            );
        }
    } else if (intype != outtype) {
        if (!state->func) {
            state->func = pgch__convert_generic;
        }

        if (intype == TEXTOID) {
            Type baseType;
            Oid baseTypeId;
            Form_pg_type typform;

            baseTypeId = getBaseTypeAndTypmod(outtype, &state->typmod);
            if (baseTypeId != INTERVALOID) {
                state->typmod = -1;
            }

            baseType          = typeidType(baseTypeId);
            typform           = (Form_pg_type)GETSTRUCT(baseType);
            state->typinput   = typform->typinput;
            state->typioparam = getTypeIOParam(baseType);
            state->func       = pgch__convert_from_text;
            ReleaseSysCache(baseType);
        } else if (outtype == BOOLOID && intype == INT2OID) {
            state->func = pgch__convert_bool;
        } else {
            state->ctype = find_coercion_pathway(
                outtype, intype, COERCION_EXPLICIT, &state->castfunc
            );
            switch (state->ctype) {
            case COERCION_PATH_FUNC:
                break;
            case COERCION_PATH_RELABELTYPE:

                /* No conversion needed unless an array rebuild is pending. */
                if (state->func == NULL) {
                    goto no_conversion;
                }
                break;
            default:
                pgch_errorf(
                    ERRCODE_FDW_INVALID_DATA_TYPE,
                    "could not cast value from %s to %s",
                    format_type_be(intype),
                    format_type_be(outtype)
                );
            }
        }
    } else if (!state->func) {
    no_conversion:
        pfree(state);
        state = NULL;
    }

    return state;
}

void*
pgch_convert_init(Datum val, Oid intype, Oid outtype) {
    return pgch__convert_init(NULL, val, intype, outtype);
}

void*
pgch_convert_init_type(const chc_type* in, Oid outtype) {
    return pgch__convert_init(in, (Datum)0, pgch_datum_oid(in), outtype);
}

void*
pgch_reader_convert_init(const pgch_reader* r, size_t col, Oid outtype) {
    if (col >= r->ncols || r->cur == NULL) {
        return NULL;
    }
    return pgch__convert_init(
        chc_block_column_type(r->cur, col), (Datum)0, r->coltypes[col], outtype
    );
}

void
pgch_reader_fill(const pgch_reader* r, void** states, Datum* values, bool* nulls) {
    for (size_t i = 0; i < r->ncols; i++) {
        nulls[i]  = r->nulls[i];
        values[i] = nulls[i] ? (Datum)0
                             : pgch_convert(states ? states[i] : NULL, r->values[i]);
    }
}

void
pgch_convert_free(void* state) {
    pfree(state);
}

char*
pgch_value_to_cstring(Oid coltype, Datum value) {
    Oid out_func;
    bool varlena;

    if (coltype == ANYARRAYOID) {
        pgch_array* slot = (pgch_array*)DatumGetPointer(value);
        void* state      = pgch_convert_init(value, ANYARRAYOID, slot->array_type);
        Datum arr        = pgch_convert(state, value);

        getTypeOutputInfo(slot->array_type, &out_func, &varlena);
        if (state) {
            pgch_convert_free(state);
        }
        return OidOutputFunctionCall(out_func, arr);
    }

    if (coltype == RECORDOID) {
        void* state = pgch_convert_init(value, RECORDOID, TEXTOID);
        Datum txt   = pgch_convert(state, value);

        if (state) {
            pgch_convert_free(state);
        }
        return TextDatumGetCString(txt);
    }

    getTypeOutputInfo(coltype, &out_func, &varlena);
    return OidOutputFunctionCall(out_func, value);
}

#endif /* PGCH_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* PG_CLICKHOUSE_DECODE_H */
