/*
 * Decode ClickHouse Native blocks into PostgreSQL Datums
 *
 * Define PGCH_IMPLEMENTATION in one translation unit. Include this header
 * without that definition everywhere else
 */

#ifndef PG_CLICKHOUSE_DECODE_H
#define PG_CLICKHOUSE_DECODE_H

#include "pg-clickhouse.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decode one value and store returned Datum type in *valtype
 * Set *valtype to JSONOID to preserve JSON text, or OID8OID on PostgreSQL 19+
 * to accept full UInt64 range
 */
extern Datum
pgch_read_value(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
);

/* Return palloc'd signature for comparing decoded Datum shapes */
extern char*
pgch_type_shape(const chc_type* type);

/*
 * Supply blocks to pgch_reader
 * Transfer block ownership from next_block to reader
 * Return NULL at end, set error callback result when stream fails
 */
typedef struct pgch_block_source {
    void* ud;
    const chc_block* (*next_block)(void* ud);
    const char* (*error)(void* ud);
} pgch_block_source;

/*
 * Supply Native byte chunks to pgch_reader_init_chunks
 * Available when implementation translation unit defines CHC_IMPLEMENTATION
 */
typedef struct pgch_chunk_source {
    void* ud;
    /*
     * Return next chunk, keep bytes valid until following call
     * Set *n to zero at end, return false and set *error on failure
     */
    bool (*next_chunk)(void* ud, const void** p, size_t* n, char** error);
    bool (*cancelled)(void* ud); /* Optional cancellation check between chunks */
} pgch_chunk_source;

/*
 * Read rows from block stream
 * Consume values before next pgch_reader_next call
 */
typedef struct pgch_reader {
    pgch_block_source src;

    Oid* coltypes; /* Returned Datum OIDs, caller may override JSON and UInt64 */
    char** colshapes;
    Datum* values;
    bool* nulls;

    size_t ncols;
    size_t row;
    const chc_block* cur;
    MemoryContext cxt;
    char* error;
    bool done;
} pgch_reader;

/*
 * Initialize reader and load first block
 * Allocate reader state in CurrentMemoryContext
 * Check reader->error when initialization leaves reader done
 */
extern void
pgch_reader_init(pgch_reader* r, const pgch_block_source* src);

/*
 * Initialize reader from Native byte chunks
 * Pass NULL opts to use pgch_block_opts_local
 */
extern void
pgch_reader_init_chunks(
    pgch_reader* r,
    const pgch_chunk_source* src,
    const chc_block_opts* opts
);

/*
 * Fill values and nulls with next row
 * Return false at end or when reader->error is set
 * Reject unsupported types and incompatible schema changes between blocks
 */
extern bool
pgch_reader_next(pgch_reader* r);

extern size_t
pgch_reader_columns(const pgch_reader* r);

/* Release current block and clear error pointer without freeing context storage */
extern void
pgch_reader_free(pgch_reader* r);

/*
 * Prepare reusable conversion from intype to outtype
 * Pass representative value for arrays and tuples
 * Return NULL when conversion is unnecessary
 * Allocate state in CurrentMemoryContext
 */
extern void*
pgch_convert_init(Datum val, Oid intype, Oid outtype);

/*
 * Prepare conversion from ClickHouse column type
 * Use pgch_reader_convert_init to read type and valtype override from reader
 */
extern void*
pgch_convert_init_type(const chc_type* in, Oid outtype);

extern void*
pgch_reader_convert_init(const pgch_reader* r, size_t col, Oid outtype);

extern Datum
pgch_convert(void* state, Datum val);

extern void
pgch_convert_free(void* state);

/* Copy current row through optional per-column conversion states */
extern void
pgch_reader_fill(const pgch_reader* r, void** states, Datum* values, bool* nulls);

/*
 * Copy current row into dest[i] of values and nulls
 * Leave unmapped positions untouched, pass NULL dest for column order
 */
extern void
pgch_reader_fill_map(
    const pgch_reader* r,
    void** states,
    const int* dest,
    Datum* values,
    bool* nulls
);

/* Return decoded value as palloc'd C string */
extern char*
pgch_value_to_cstring(Oid coltype, Datum value);

#ifdef PGCH_IMPLEMENTATION

#include <string.h>
#include <sys/socket.h> /* PostgreSQL inet macros require AF_INET */

#include "access/htup_details.h"
#include "access/tupconvert.h"
#include "catalog/pg_type_d.h"
#include "common/int.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "parser/parse_coerce.h"
#include "port/pg_bswap.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
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

/* ClickHouse stores Decimal as a scaled, little-endian signed integer */
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

/* ClickHouse stores IPv4 as native uint32, PostgreSQL inet uses network order */
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

/* ClickHouse and PostgreSQL store IPv6 in network order */
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

/* PGCH_NATIVE_SETTINGS makes ClickHouse serialize JSON as document text */
static Datum
pgch__read_json(const chc_column* col, uint64_t row, Oid valtype) {
    const char* p;
    size_t len;
    char* cstr;
    Datum ret;

    pgch__slice_str(col, row, &p, &len);
    cstr = pnstrdup(p, len);
    ret  = DirectFunctionCall1(
        valtype == JSONOID ? json_in : jsonb_in, CStringGetDatum(cstr)
    );
    pfree(cstr);
    return ret;
}

/* Nullable LowCardinality reserves dictionary entry zero for NULL */
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

    /* PostgreSQL uses one array type for every nesting depth */
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

    chc_kind kind = chc_type_kind(type);
    Oid want      = *valtype;

    *valtype = pgch_kind_oids[kind];
    *is_null = false;

    switch (kind) {
    case CHC_VOID:
    case CHC_NOTHING:
        *is_null = true;
        return (Datum)0;
    case CHC_UINT8:
        return (Datum)pgch__rd_u8(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_BOOL:
        return (Datum)pgch__rd_bool((const bool*)chc_column_fixed_data(col, NULL), row);
    case CHC_INT8:
        return (Datum)pgch__rd_i8(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_INT16:
        return (Datum)pgch__rd_i16(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_UINT16:
        return (Datum)pgch__rd_u16(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_INT32:
        return (Datum)pgch__rd_i32(
            (const uint8_t*)chc_column_fixed_data(col, NULL), row
        );
    case CHC_UINT32:
        return Int64GetDatum(
            (int64)pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT64:
        return Int64GetDatum(
            pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_UINT64: {
        uint64_t v =
            pgch__rd_u64((const uint8_t*)chc_column_fixed_data(col, NULL), row);

#if PG_VERSION_NUM >= 190000
        /* PostgreSQL oid8 supports full UInt64 range */
        if (want == OID8OID) {
            *valtype = OID8OID;
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
        return Int64GetDatum((int64)v);
    }
    case CHC_FLOAT32:
        return Float4GetDatum(
            pgch__rd_f32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_FLOAT64:
        return Float8GetDatum(
            pgch__rd_f64((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_DECIMAL32:
    case CHC_DECIMAL64:
    case CHC_DECIMAL128:
    case CHC_DECIMAL256:
        return pgch__read_decimal(col, type, row);
    case CHC_STRING:
        return pgch__read_string(col, row);
    case CHC_ENUM8:
    case CHC_ENUM16:
        return pgch__read_enum(col, type, row);
    case CHC_JSON:
    case CHC_OBJECT:
        if (want == JSONOID) {
            *valtype = JSONOID;
        }
        return pgch__read_json(col, row, *valtype);
    case CHC_FIXED_STRING:
        return pgch__read_fixedstring(col, row);
    case CHC_DATE:
        return DateADTGetDatum(
            (DateADT)
                pgch__rd_u16((const uint8_t*)chc_column_fixed_data(col, NULL), row) -
            PGCH__DATE_OFFSET
        );
    case CHC_DATE32:
        return DateADTGetDatum(
            (DateADT)
                pgch__rd_i32((const uint8_t*)chc_column_fixed_data(col, NULL), row) -
            PGCH__DATE_OFFSET
        );
    case CHC_DATETIME: {
        uint32_t secs =
            pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row);

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
        TimestampTz ts;

        if (pg_mul_s64_overflow(raw / power, USECS_PER_SEC, &ts) ||
            pg_sub_s64_overflow(
                ts, PGCH__DATE_OFFSET * SECS_PER_DAY * USECS_PER_SEC, &ts
            ) ||
            pg_add_s64_overflow(ts, (raw % power) * USECS_PER_SEC / power, &ts) ||
            !IS_VALID_TIMESTAMP(ts)) {
            pgch_error(
                ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "DateTime64 value out of range"
            );
        }
        return TimestampTzGetDatum(ts);
    }
    case CHC_TIME: {
        const uint8_t* p = (const uint8_t*)chc_column_fixed_data(col, NULL);

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
        TimeADT t;

        if (pg_mul_s64_overflow(raw / power, USECS_PER_SEC, &t) ||
            pg_add_s64_overflow(t, (raw % power) * USECS_PER_SEC / power, &t)) {
            pgch_error(
                ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "Time64 value out of range"
            );
        }
        return TimeADTGetDatum(t);
    }
    case CHC_UUID:
        return pgch__read_uuid(col, row);
    case CHC_IPV4:
        return pgch__read_ipv4(col, row);
    case CHC_IPV6:
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

static const char*
pgch__check_type(const chc_type* type) {
    if (chc_type_kind(type) == CHC_NULLABLE) {
        type = chc_type_child(type, 0);
    }

    switch (chc_type_kind(type)) {
    case CHC_VOID:
    case CHC_NOTHING:
        return NULL;
    case CHC_LOW_CARDINALITY: {
        const chc_type* inner = chc_type_child(type, 0);

        /* ClickHouse nests Nullable inside LowCardinality */
        if (chc_type_kind(inner) == CHC_NULLABLE) {
            inner = chc_type_child(inner, 0);
        }
        if (chc_type_kind(inner) != CHC_STRING) {
            return "unsupported LowCardinality inner type";
        }
        return NULL;
    }
    case CHC_ARRAY: {
        const chc_type* leaf = type;
        const char* msg      = pgch__check_type(chc_type_child(type, 0));

        if (msg) {
            return msg;
        }
        while (chc_type_kind(leaf) == CHC_ARRAY) {
            leaf = chc_type_child(leaf, 0);
        }
        if (!OidIsValid(get_array_type(pgch_datum_oid(leaf)))) {
            return psprintf(
                "no PG array type for column type \"%s\"", chc_type_name(leaf, NULL)
            );
        }
        return NULL;
    }
    case CHC_TUPLE: {
        size_t n = chc_type_n_children(type);

        if (n == 0) {
            return "returned tuple is empty";
        }
        for (size_t i = 0; i < n; i++) {
            const char* msg = pgch__check_type(chc_type_child(type, i));

            if (msg) {
                return msg;
            }
        }
        return NULL;
    }
    default:
        if (!OidIsValid(pgch_kind_oids[chc_type_kind(type)])) {
            return psprintf(
                "unsupported column type \"%s\"", chc_type_name(type, NULL)
            );
        }
        return NULL;
    }
}

static const char*
pgch__block_col_desc(const chc_block* b, size_t i) {
    size_t len;
    const char* name = chc_block_column_name(b, i, &len);

    return len ? psprintf("column \"%.*s\"", (int)len, name)
               : psprintf("column %zu", i + 1);
}

/* Preserve errors when caller resets row context */
static bool
pgch__schema_error(pgch_reader* r, const char* msg) {
    r->error = MemoryContextStrdup(r->cxt, msg);
    return false;
}

static bool
pgch__check_schema(pgch_reader* r) {
    size_t ncols = chc_block_n_columns(r->cur);

    if (r->colshapes) {
        if (ncols != r->ncols) {
            return pgch__schema_error(
                r,
                psprintf("block column count changed from %zu to %zu", r->ncols, ncols)
            );
        }
        for (size_t i = 0; i < ncols; i++) {
            char* shape = pgch_type_shape(chc_block_column_type(r->cur, i));
            bool match  = strcmp(shape, r->colshapes[i]) == 0;

            pfree(shape);
            if (!match) {
                return pgch__schema_error(
                    r, psprintf("block column %zu type changed", i + 1)
                );
            }
        }
    }

    for (size_t i = 0; i < ncols; i++) {
        const char* msg = pgch__check_type(chc_block_column_type(r->cur, i));

        if (msg) {
            return pgch__schema_error(
                r, psprintf("%s (%s)", msg, pgch__block_col_desc(r->cur, i))
            );
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
    if (!pgch__check_schema(r)) {
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

    const uint8_t* cur; /* Borrowed until next next_chunk call */
    size_t len;
    size_t pos;
    char* error;
    bool eos;
} pgch__chunks;

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
        c->error =
            MemoryContextStrdup(c->cxt, err.msg[0] ? err.msg : "block read failed");
        return NULL;
    }
    return b;
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

    /* First block defines stream schema, even when it contains no rows */
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

    while (r->row >= chc_block_n_rows(r->cur)) {
        r->row = 0;
        if (!pgch__load_block(r)) {
            return false;
        }
    }

    for (size_t i = 0; i < ncols; i++) {
        Oid t                 = r->coltypes[i];
        const chc_column* col = chc_block_column(r->cur, i);
        const chc_type* ct    = chc_block_column_type(r->cur, i);

        r->values[i] = pgch_read_value(col, ct, r->row, &t, &r->nulls[i]);
    }

    r->row++;
    return true;
}

void
pgch_reader_free(pgch_reader* r) {
    if (r->cur) {
        chc_block_destroy(unconstify(chc_block*, r->cur), &pgch_alloc);
        r->cur = NULL;
    }
    r->error = NULL;
}

/* ---- conversion ----------------------------------------------------- */

typedef struct pgch_convert_state pgch_convert_state;
typedef Datum (*pgch__convert_fn)(pgch_convert_state*, Datum);

struct pgch_convert_state {
    Oid intype;
    Oid outtype;
    pgch__convert_fn func;
    FmgrInfo flinfo; /* Cast, type input or record_out, whichever func needs */

    TupleConversionMap* tupmap;
    TupleDesc indesc;
    TupleDesc outdesc;
    pgch_convert_state** field_states;

    Oid item_type;
    pgch_convert_state* elem_state;
    int16 typlen;
    bool typbyval;
    char typalign;

    int32 typmod;
    Oid typioparam;

    CoercionPathType ctype;
};

static inline Datum
pgch__convert_generic(pgch_convert_state* state, Datum val) {
    if (state->ctype == COERCION_PATH_FUNC) {
        Assert(OidIsValid(state->flinfo.fn_oid));
        val = FunctionCall1(&state->flinfo, val);
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

        if (slot->nulls[i]) {
            continue;
        }

        if (s == NULL && slot->types[i] == RECORDOID) {
            MemoryContext oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(state));

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
            val = CStringGetTextDatum(
                DatumGetCString(FunctionCall1(&state->flinfo, val))
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

            /* PostgreSQL arrays are rectangular, ClickHouse nested arrays are not */
            if (!pgch__flatten_array(slot, dims, 0, flat, flatnulls, &idx)) {
                pgch_error(
                    ERRCODE_ARRAY_SUBSCRIPT_ERROR,
                    "nested arrays must have sub-arrays with matching dimensions"
                );
            }
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
        }
    }

    return pgch__convert_generic(state, val);
}

static Datum
pgch__convert_from_text(pgch_convert_state* state, Datum val) {
    return InputFunctionCall(
        &state->flinfo, TextDatumGetCString(val), state->typioparam, state->typmod
    );
}

/* ClickHouse UInt8 maps to smallint, convert requested boolean explicitly */
static Datum
pgch__convert_bool(pgch_convert_state* state pg_attribute_unused(), Datum val) {
    return BoolGetDatum(DatumGetInt16(val));
}

Datum
pgch_convert(void* state, Datum val) {
    return state ? ((pgch_convert_state*)state)->func(state, val) : val;
}

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

static pgch_convert_state*
pgch__convert_init(const chc_type* ct, Datum val, Oid intype, Oid outtype) {
    if (intype == TEXTOID && outtype == BYTEAOID) {
        return NULL;
    }
    /* ClickHouse Nothing and Void values are always NULL */
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
        /* PostgreSQL array domains expose element type through base type */
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

        /* PostgreSQL reports array casts separately from scalar element casts */
        if (OidIsValid(out_elem) && out_elem != state->item_type) {
            Datum leafval  = (Datum)0;
            bool have_leaf = ct || pgch__array_leaf(slot, &leafval);

            if (have_leaf || state->item_type != RECORDOID) {
                state->elem_state =
                    pgch__convert_init(leaf, leafval, state->item_type, out_elem);
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

        if (outtype == TEXTOID) {
            fmgr_info(F_RECORD_OUT, &state->flinfo);
        } else if (outtype != RECORDOID) {
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
            Oid baseTypeId = getBaseTypeAndTypmod(outtype, &state->typmod);
            Oid typinput;

            if (baseTypeId != INTERVALOID) {
                state->typmod = -1;
            }

            getTypeInputInfo(baseTypeId, &typinput, &state->typioparam);
            fmgr_info(typinput, &state->flinfo);
            state->func = pgch__convert_from_text;
        } else if (outtype == BOOLOID && intype == INT2OID) {
            state->func = pgch__convert_bool;
        } else {
            Oid castfunc;

            state->ctype =
                find_coercion_pathway(outtype, intype, COERCION_EXPLICIT, &castfunc);
            switch (state->ctype) {
            case COERCION_PATH_FUNC:
                fmgr_info(castfunc, &state->flinfo);
                break;
            case COERCION_PATH_RELABELTYPE:

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
pgch_reader_fill_map(
    const pgch_reader* r,
    void** states,
    const int* dest,
    Datum* values,
    bool* nulls
) {
    for (size_t i = 0; i < r->ncols; i++) {
        int d = dest ? dest[i] : (int)i;

        nulls[d] = r->nulls[i];
        values[d] =
            nulls[d] ? (Datum)0 : pgch_convert(states ? states[i] : NULL, r->values[i]);
    }
}

void
pgch_reader_fill(const pgch_reader* r, void** states, Datum* values, bool* nulls) {
    pgch_reader_fill_map(r, states, NULL, values, nulls);
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
