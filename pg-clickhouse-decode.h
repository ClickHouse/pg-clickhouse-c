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
 * Set *valtype to JSONOID to preserve JSON text
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
 * Reject unsupported types, incompatible schema changes between blocks, and
 * columns failing chc_column_validate
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
 * Pass target type modifier to enforce length and precision, or -1 for none
 * Return NULL when conversion is unnecessary
 * Allocate state in CurrentMemoryContext
 */
extern void*
pgch_convert_init(Datum val, Oid intype, Oid outtype, int32 outtypmod);

/*
 * Prepare conversion from ClickHouse column type
 * Use pgch_reader_convert_init to read type and valtype override from reader
 */
extern void*
pgch_convert_init_type(const chc_type* in, Oid outtype, int32 outtypmod);

extern void*
pgch_reader_convert_init(
    const pgch_reader* r,
    size_t col,
    Oid outtype,
    int32 outtypmod
);

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
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/geo_decls.h"
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

/*
 * Generate fixed-width scalar readers. For example, i16 invocation below
 * defines pgch__rd_i16(), which reads two bytes at row offset, converts them
 * from little-endian to host order, and returns int16_t
 */
#define PGCH__RD_FIXED(suffix, T, U, LE)                                               \
    static inline T pgch__rd_##suffix(const uint8_t* p, uint64_t row) {                \
        T v;                                                                           \
        U u;                                                                           \
                                                                                       \
        memcpy(&u, p + row * sizeof(T), sizeof u);                                     \
        u = LE(u);                                                                     \
        memcpy(&v, &u, sizeof v);                                                      \
        return v;                                                                      \
    }

PGCH__RD_FIXED(i16, int16_t, uint16_t, PGCH__LE16)
PGCH__RD_FIXED(u16, uint16_t, uint16_t, PGCH__LE16)
PGCH__RD_FIXED(i32, int32_t, uint32_t, PGCH__LE32)
PGCH__RD_FIXED(u32, uint32_t, uint32_t, PGCH__LE32)
PGCH__RD_FIXED(i64, int64_t, uint64_t, PGCH__LE64)
PGCH__RD_FIXED(f32, float, uint32_t, PGCH__LE32)
PGCH__RD_FIXED(f64, double, uint64_t, PGCH__LE64)
PGCH__RD_FIXED(u64, uint64_t, uint64_t, PGCH__LE64)

/* BFloat16 holds the leading 16 bits of a Float32, so widening fills with zeros */
static inline float
pgch__rd_bf16(const uint8_t* p, uint64_t row) {
    uint32_t bits = (uint32_t)pgch__rd_u16(p, row) << 16;
    float v;

    memcpy(&v, &bits, sizeof v);
    return v;
}

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

/* ClickHouse stores Decimal and wide integers as little-endian two's complement */
static int
pgch__format_int(
    const uint8_t* bytes,
    size_t width,
    uint32_t scale,
    bool is_signed,
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
    for (size_t i = 0; i < nwords; i++) {
        mag[i] = pgch__rd_u32(bytes, i);
    }
    if (is_signed && (mag[nwords - 1] & 0x80000000u)) {
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

    if (pgch__format_int(p + row * es, es, scale, true, buf, sizeof(buf)) < 0) {
        pgch_error(ERRCODE_FDW_ERROR, "decimal too wide");
    }
    return DirectFunctionCall3(
        numeric_in, CStringGetDatum(buf), ObjectIdGetDatum(0), Int32GetDatum(-1)
    );
}

/* ClickHouse integers outside bigint's range map to PostgreSQL numeric */
static Datum
pgch__read_wide_int(const chc_column* col, chc_kind kind, uint64_t row) {
    size_t width;
    const uint8_t* p = (const uint8_t*)chc_column_fixed_data(col, &width);
    char buf[80];

#if PG_VERSION_NUM >= 140000
    if (kind == CHC_UINT64) {
        uint64_t v = pgch__rd_u64(p, row);

        if (v <= (uint64_t)PG_INT64_MAX) {
            return NumericGetDatum(int64_to_numeric((int64)v));
        }
    }
#endif
    if (pgch__format_int(
            p + row * width, width, 0, !pgch_kind_is_unsigned(kind), buf, sizeof(buf)
        ) < 0) {
        pgch_error(ERRCODE_FDW_ERROR, "integer too wide");
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
    pg_verifymbstr(p, (int)len, false);
    return PointerGetDatum(cstring_to_text_with_len(p, len));
}

static Datum
pgch__read_fixedstring(const chc_column* col, uint64_t row) {
    size_t width;
    const uint8_t* base = chc_column_fixed_data(col, &width);
    pg_verifymbstr((const char*)base, (int)strnlen((const char*)base, width), false);

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
    a = pg_bswap64(a);
    b = pg_bswap64(b);
    memcpy(u->data, &a, 8);
    memcpy(u->data + 8, &b, 8);
    return UUIDPGetDatum(u);
}

/* ClickHouse is one unit, PostgreSQL keeps months, days and time apart */
static Datum
pgch__read_interval(const chc_column* col, const chc_type* type, uint64_t row) {
    int64 raw     = pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row);
    Interval unit = pgch_interval_unit_of(type);
    /* Exactly one field is set, or all zero for nanoseconds */
    int64 scale  = unit.month + unit.day + unit.time;
    Interval* iv = (Interval*)palloc0(sizeof(Interval));
    int64 v      = raw / 1000;

    if (scale && (pg_mul_s64_overflow(raw, scale, &v) ||
                  ((unit.month || unit.day) && v != (int32)v))) {
        pgch_errorf(
            ERRCODE_DATETIME_VALUE_OUT_OF_RANGE,
            "%s value out of range",
            chc_type_name(type, NULL)
        );
    }
    if (unit.month) {
        iv->month = (int32)v;
    } else if (unit.day) {
        iv->day = (int32)v;
    } else {
        iv->time = v;
    }
    return IntervalPGetDatum(iv);
}

/* ClickHouse Point is Tuple(Float64, Float64), one column per axis */
static Datum
pgch__read_point(const chc_column* col, uint64_t row) {
    Point* p = (Point*)palloc(sizeof(Point));

    p->x = pgch__rd_f64(
        (const uint8_t*)chc_column_fixed_data(chc_column_tuple_child(col, 0), NULL), row
    );
    p->y = pgch__rd_f64(
        (const uint8_t*)chc_column_fixed_data(chc_column_tuple_child(col, 1), NULL), row
    );
    return PointPGetDatum(p);
}

/* Ring and LineString are Array(Point), so a row is a slice of the axis columns */
static int
pgch__read_axes(
    const chc_column* col,
    uint64_t row,
    const uint8_t** xs,
    const uint8_t** ys
) {
    const uint64_t* offs  = chc_column_array_offsets(col);
    uint64_t start        = row == 0 ? 0 : offs[row - 1];
    uint64_t npts         = offs[row] - start;
    const chc_column* pts = chc_column_array_values(col);

    /* PostgreSQL counts points in an int32 and stores them inline */
    if (npts > (INT_MAX - offsetof(POLYGON, p)) / sizeof(Point)) {
        pgch_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED, "too many points requested");
    }
    *xs = (const uint8_t*)chc_column_fixed_data(chc_column_tuple_child(pts, 0), NULL) +
          start * sizeof(double);
    *ys = (const uint8_t*)chc_column_fixed_data(chc_column_tuple_child(pts, 1), NULL) +
          start * sizeof(double);
    return (int)npts;
}

static void
pgch__fill_points(Point* out, const uint8_t* xs, const uint8_t* ys, int npts) {
    for (int i = 0; i < npts; i++) {
        out[i].x = pgch__rd_f64(xs, i);
        out[i].y = pgch__rd_f64(ys, i);
    }
}

/* make_bound_box from src/backend/utils/adt/geo_ops.c, static there */
static void
pgch__bound_box(POLYGON* poly) {
    float8 x1 = poly->p[0].x, x2 = x1;
    float8 y1 = poly->p[0].y, y2 = y1;

    for (int i = 1; i < poly->npts; i++) {
        if (float8_lt(poly->p[i].x, x1)) {
            x1 = poly->p[i].x;
        }
        if (float8_gt(poly->p[i].x, x2)) {
            x2 = poly->p[i].x;
        }
        if (float8_lt(poly->p[i].y, y1)) {
            y1 = poly->p[i].y;
        }
        if (float8_gt(poly->p[i].y, y2)) {
            y2 = poly->p[i].y;
        }
    }
    poly->boundbox.low.x  = x1;
    poly->boundbox.high.x = x2;
    poly->boundbox.low.y  = y1;
    poly->boundbox.high.y = y2;
}

/* A Ring is closed by definition, as a PostgreSQL polygon is */
static Datum
pgch__read_polygon(const chc_column* col, uint64_t row, bool* is_null) {
    const uint8_t *xs, *ys;
    int npts = pgch__read_axes(col, row, &xs, &ys);

    /* PostgreSQL has no pointless polygon, so an empty ring reads as NULL */
    if (npts == 0) {
        *is_null = true;
        return (Datum)0;
    }
    size_t size   = offsetof(POLYGON, p) + sizeof(Point) * npts;
    POLYGON* poly = (POLYGON*)palloc0(size);
    SET_VARSIZE(poly, size);
    poly->npts = npts;
    pgch__fill_points(poly->p, xs, ys, npts);
    pgch__bound_box(poly);
    return PolygonPGetDatum(poly);
}

/* A LineString repeating its first point is the closed path that wrote it */
static Datum
pgch__read_path(const chc_column* col, uint64_t row, bool* is_null) {
    const uint8_t *xs, *ys;
    int npts    = pgch__read_axes(col, row, &xs, &ys);
    bool closed = npts > 1 &&
                  float8_eq(pgch__rd_f64(xs, 0), pgch__rd_f64(xs, npts - 1)) &&
                  float8_eq(pgch__rd_f64(ys, 0), pgch__rd_f64(ys, npts - 1));

    npts -= closed;
    /* PostgreSQL has no pointless path, so an empty line reads as NULL */
    if (npts == 0) {
        *is_null = true;
        return (Datum)0;
    }
    size_t size = offsetof(PATH, p) + sizeof(Point) * npts;
    PATH* path  = (PATH*)palloc0(size);
    SET_VARSIZE(path, size);
    path->npts   = npts;
    path->closed = closed;
    pgch__fill_points(path->p, xs, ys, npts);
    return PathPGetDatum(path);
}

/* ClickHouse stores IPv4 as native uint32, PostgreSQL inet uses network order */
static Datum
pgch__read_ipv4(const chc_column* col, uint64_t row) {
    inet* res = (inet*)palloc0(sizeof(inet));
    uint32_t addr =
        pg_hton32(pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row));

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
        v = pgch__rd_i16(p, row);
    }

    size_t n = chc_type_enum_count(type);

    for (size_t i = 0; i < n; i++) {
        const char* en;
        size_t el;
        int64_t ev;

        chc_type_enum_at(type, i, &en, &el, &ev);
        pg_verifymbstr(en, (int)el, false);
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

    pgch__slice_str(col, row, &p, &len);
    pg_verifymbstr(p, (int)len, false);
    char* cstr = pnstrdup(p, len);
    Datum ret  = DirectFunctionCall1(
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

/*
 * Walk an array shaped type to element pgch_array, reporting how many
 * PostgreSQL dimensions it spans and type walk stopped on.
 * Includes CH types which are represented by arrays in native format.
 */
static Oid
pgch__array_item(const chc_type* type, const chc_type** leaf, int* ndim) {
    int dims = 0;

    for (;;) {
        type  = pgch_unwrap(type, NULL);
        *leaf = type;
        switch (chc_type_kind(type)) {
        case CHC_ARRAY:
            dims++;
            type = chc_type_child(type, 0);
            continue;
        /* Map holds Tuple(K, V) pairs */
        case CHC_MAP:
            *ndim = dims + 1;
            return RECORDOID;
        case CHC_POLYGON:
            *ndim = dims + 1;
            return pgch_kind_oids[CHC_RING];
        /* MultiPolygon nests its rings one level deeper than Polygon */
        case CHC_MULTI_POLYGON:
            *ndim = dims + 2;
            return pgch_kind_oids[CHC_RING];
        case CHC_MULTI_LINE_STRING:
            *ndim = dims + 1;
            return pgch_kind_oids[CHC_LINE_STRING];
        default:
            *ndim = dims;
            return pgch_datum_oid(type);
        }
    }
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
    const chc_type* leaf;
    int ndim;

    slot->len = len;
    /* PostgreSQL uses one array type for every nesting depth */
    slot->item_type  = pgch__array_item(type, &leaf, &ndim);
    slot->ndim       = ndim;
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

/* Geo types carry no children, so a level's element kind follows from the kind */
static chc_kind
pgch__geo_child(chc_kind kind) {
    switch (kind) {
    case CHC_MULTI_POLYGON:
        return CHC_POLYGON;
    case CHC_POLYGON:
        return CHC_RING;
    case CHC_MULTI_LINE_STRING:
        return CHC_LINE_STRING;
    case CHC_RING:
    case CHC_LINE_STRING:
        return CHC_POINT;
    default:
        pg_unreachable();
    }
}

static Datum
pgch__read_geo(
    const chc_column* col,
    chc_kind kind,
    uint64_t row,
    Oid* valtype,
    bool* is_null
);

/* PostgreSQL has no multi-geometry types, so their members become array items */
static Datum
pgch__read_geo_array(
    const chc_column* col,
    chc_kind kind,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    const uint64_t* offs    = chc_column_array_offsets(col);
    uint64_t start          = row == 0 ? 0 : offs[row - 1];
    uint64_t len            = offs[row] - start;
    const chc_column* inner = chc_column_array_values(col);
    chc_kind child          = pgch__geo_child(kind);
    pgch_array* slot        = (pgch_array*)palloc(sizeof(pgch_array));

    slot->len  = len;
    slot->ndim = kind == CHC_MULTI_POLYGON ? 2 : 1;
    slot->item_type =
        pgch_kind_oids[kind == CHC_MULTI_LINE_STRING ? CHC_LINE_STRING : CHC_RING];
    slot->array_type = get_array_type(slot->item_type);
    slot->datums     = len ? (Datum*)palloc0(sizeof(Datum) * len) : NULL;
    slot->nulls      = len ? (bool*)palloc0(sizeof(bool) * len) : NULL;

    for (uint64_t i = 0; i < len; i++) {
        Oid scratch = slot->item_type;

        slot->datums[i] =
            pgch__read_geo(inner, child, start + i, &scratch, &slot->nulls[i]);
    }

    *valtype = ANYARRAYOID;
    *is_null = false;
    return PointerGetDatum(slot);
}

static Datum
pgch__read_geo(
    const chc_column* col,
    chc_kind kind,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    *valtype = pgch_kind_oids[kind];
    switch (kind) {
    case CHC_POINT:
        return pgch__read_point(col, row);
    case CHC_RING:
        return pgch__read_polygon(col, row, is_null);
    case CHC_LINE_STRING:
        return pgch__read_path(col, row, is_null);
    case CHC_POLYGON:
    case CHC_MULTI_POLYGON:
    case CHC_MULTI_LINE_STRING:
        return pgch__read_geo_array(col, kind, row, valtype, is_null);
    default:
        pg_unreachable();
    }
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

    if (n == 0) {
        pgch_error(ERRCODE_FDW_ERROR, "returned tuple is empty");
    }

    pgch_tuple* slot   = (pgch_tuple*)palloc(sizeof(pgch_tuple));
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

/*
 * Map is Array(Tuple(K, V)) carrying no Tuple type of its own, so the pair
 * reads against the Map type, whose two children are the field types
 */
static Datum
pgch__read_map(
    const chc_column* col,
    const chc_type* type,
    uint64_t row,
    Oid* valtype,
    bool* is_null
) {
    const uint64_t* offs      = chc_column_array_offsets(col);
    uint64_t start            = row == 0 ? 0 : offs[row - 1];
    uint64_t len              = offs[row] - start;
    const chc_column* entries = chc_column_array_values(col);
    pgch_array* slot          = (pgch_array*)palloc(sizeof(pgch_array));

    slot->len        = len;
    slot->ndim       = 1;
    slot->item_type  = RECORDOID;
    slot->array_type = RECORDARRAYOID;
    slot->datums     = len ? (Datum*)palloc0(sizeof(Datum) * len) : NULL;
    slot->nulls      = len ? (bool*)palloc0(sizeof(bool) * len) : NULL;

    for (uint64_t i = 0; i < len; i++) {
        Oid scratch = RECORDOID;

        slot->datums[i] =
            pgch__read_tuple(entries, type, start + i, &scratch, &slot->nulls[i]);
    }

    *valtype = ANYARRAYOID;
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
        return Int16GetDatum(
            pgch__rd_u8((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_BOOL:
        return BoolGetDatum(
            pgch__rd_bool((const bool*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT8:
        return Int16GetDatum(
            pgch__rd_i8((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT16:
        return Int16GetDatum(
            pgch__rd_i16((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_UINT16:
        return Int32GetDatum(
            pgch__rd_u16((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT32:
        return Int32GetDatum(
            pgch__rd_i32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_UINT32:
        return Int64GetDatum(
            (int64)pgch__rd_u32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_INT64:
        return Int64GetDatum(
            pgch__rd_i64((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_UINT64:
    case CHC_INT128:
    case CHC_UINT128:
    case CHC_INT256:
    case CHC_UINT256:
        return pgch__read_wide_int(col, kind, row);
    case CHC_FLOAT32:
        return Float4GetDatum(
            pgch__rd_f32((const uint8_t*)chc_column_fixed_data(col, NULL), row)
        );
    case CHC_BFLOAT16:
        return Float4GetDatum(
            pgch__rd_bf16((const uint8_t*)chc_column_fixed_data(col, NULL), row)
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
    case CHC_DATE32: {
        int32 days =
            pgch__rd_i32((const uint8_t*)chc_column_fixed_data(col, NULL), row);
        DateADT d;

        if (pg_sub_s32_overflow(days, PGCH__DATE_OFFSET, &d) || !IS_VALID_DATE(d)) {
            pgch_error(
                ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "Date32 value out of range"
            );
        }
        return DateADTGetDatum(d);
    }
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
        TimeADT t        = (TimeADT)pgch__rd_i32(p, row) * USECS_PER_SEC;

        /* ClickHouse Time allows up to 999 hours, PostgreSQL time allows one day */
        if (t < 0 || t > USECS_PER_DAY) {
            pgch_error(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "Time value out of range");
        }
        return TimeADTGetDatum(t);
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
            pg_add_s64_overflow(t, (raw % power) * USECS_PER_SEC / power, &t) ||
            t < 0 || t > USECS_PER_DAY) {
            pgch_error(
                ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "Time64 value out of range"
            );
        }
        return TimeADTGetDatum(t);
    }
    case CHC_INTERVAL:
        return pgch__read_interval(col, type, row);
    case CHC_UUID:
        return pgch__read_uuid(col, row);
    case CHC_POINT:
    case CHC_RING:
    case CHC_LINE_STRING:
    case CHC_POLYGON:
    case CHC_MULTI_POLYGON:
    case CHC_MULTI_LINE_STRING:
        return pgch__read_geo(col, kind, row, valtype, is_null);
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
    case CHC_MAP:
        return pgch__read_map(col, type, row, valtype, is_null);
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
    /* Map reads as Array(Tuple(K, V)), so it shares that shape */
    case CHC_MAP:
        appendStringInfoChar(buf, 'a');
        /* fall through */
        CHC_FALLTHROUGH;
    case CHC_TUPLE: {
        size_t n = chc_type_n_children(type);

        appendStringInfo(buf, "t%zu(", n);
        for (size_t i = 0; i < n; i++) {
            pgch__append_shape(buf, chc_type_child(type, i));
        }
        appendStringInfoChar(buf, ')');
        return;
    }
    /* Multi-geometry types share an array Datum, so the kind separates them */
    case CHC_POLYGON:
    case CHC_MULTI_POLYGON:
    case CHC_MULTI_LINE_STRING:
        appendStringInfo(buf, "g%d;", (int)kind);
        return;
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
        const chc_type* leaf;
        int ndim;
        const char* msg = pgch__check_type(chc_type_child(type, 0));

        if (msg) {
            return msg;
        }
        if (!OidIsValid(get_array_type(pgch__array_item(type, &leaf, &ndim)))) {
            return psprintf(
                "no PG array type for column type \"%s\"", chc_type_name(leaf, NULL)
            );
        }
        return NULL;
    }
    case CHC_MAP:
        if (chc_type_n_children(type) != 2) {
            return "returned map wants key and value";
        }
        /* fall through */
        CHC_FALLTHROUGH;
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
    /* Nodeless geo arrays over Ring or LineString, both of which PostgreSQL has */
    case CHC_POLYGON:
    case CHC_MULTI_POLYGON:
    case CHC_MULTI_LINE_STRING:
        return NULL;
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
pgch__check_block(pgch_reader* r) {
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
        chc_err err     = {};

        if (msg) {
            return pgch__schema_error(
                r, psprintf("%s (%s)", msg, pgch__block_col_desc(r->cur, i))
            );
        }
        /* chc_block_read leaves offsets and keys unchecked, decode trusts them */
        if (chc_column_validate(chc_block_column(r->cur, i), &err) != CHC_OK) {
            return pgch__schema_error(
                r,
                psprintf(
                    "%s (%s)",
                    err.msg[0] ? err.msg : "invalid column data",
                    pgch__block_col_desc(r->cur, i)
                )
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
    if (!pgch__check_block(r)) {
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

/* Decode into init context, block outlives caller's per-row context */
static const chc_block*
pgch__chunk_next_block(void* ud) {
    pgch__chunks* c = (pgch__chunks*)ud;
    chc_block* b    = NULL;
    chc_err err     = {};

    if (c->error) {
        return NULL;
    }
    MemoryContext old = MemoryContextSwitchTo(c->cxt);
    if (chc_block_read(c->in, &pgch_alloc, &c->opts, &b, &err) != CHC_OK) {
        c->error =
            MemoryContextStrdup(c->cxt, err.msg[0] ? err.msg : "block read failed");
        b = NULL;
    }
    MemoryContextSwitchTo(old);
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
        pgch_raise(&err, ERRCODE_FDW_ERROR, "reader init: ", NULL);
    }

    bsrc.ud         = c;
    bsrc.next_block = pgch__chunk_next_block;
    bsrc.error      = pgch__chunk_error;
    pgch_reader_init(r, &bsrc);
}

#endif /* CHC_IMPLEMENTATION */

void
pgch_reader_init(pgch_reader* r, const pgch_block_source* src) {
    memset(r, 0, sizeof(*r));
    r->src = *src;
    r->cxt = CurrentMemoryContext;

    const char* src_err = r->src.error(r->src.ud);
    if (src_err) {
        r->done  = true;
        r->error = pstrdup(src_err);
        return;
    }

    /* First block defines stream schema, even when it contains no rows */
    if (!pgch__load_block(r)) {
        return;
    }

    size_t ncols = chc_block_n_columns(r->cur);
    r->ncols     = ncols;
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
    if (r->done || r->coltypes == NULL || r->error) {
        return false;
    }

    size_t ncols = r->ncols;

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
    FmgrInfo flinfo;    /* Function used for cast, type input, or record output */
    FmgrInfo tmflinfo;  /* Function used to enforce target length or precision */
    FmgrInfo outflinfo; /* Value's output function when casting through text */

    TupleConversionMap* tupmap;
    TupleDesc indesc;
    TupleDesc outdesc;
    pgch_convert_state** field_states;

    void* domain_extra;        /* Cache for target domain checks */
    pgch_convert_state* inner; /* Converts values to domain's base type */

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
        val = pgch__cast_call(&state->flinfo, val, state->typmod, false);
    }
    if (OidIsValid(state->tmflinfo.fn_oid)) {
        val = pgch__cast_call(&state->tmflinfo, val, state->typmod, false);
    }

    return val;
}

static Datum
pgch__convert_record(pgch_convert_state* state, Datum val) {
    pgch_tuple* slot = (pgch_tuple*)DatumGetPointer(val);

    for (size_t i = 0; i < slot->len; i++) {
        pgch_convert_state* s = state->field_states[i];

        if (slot->nulls[i]) {
            continue;
        }

        if (s == NULL && slot->types[i] == RECORDOID) {
            MemoryContext oldcxt  = MemoryContextSwitchTo(GetMemoryChunkContext(state));
            Form_pg_attribute att = TupleDescAttr(state->indesc, i);

            s = pgch_convert_init(
                slot->datums[i], RECORDOID, att->atttypid, att->atttypmod
            );
            MemoryContextSwitchTo(oldcxt);
            state->field_states[i] = s;
        }

        if (s) {
            slot->datums[i] = s->func(s, slot->datums[i]);
        }
    }

    HeapTuple htup = heap_form_tuple(state->indesc, slot->datums, slot->nulls);
    if (!state->outdesc) {
        val = heap_copy_tuple_as_datum(htup, state->indesc);

        if (state->outtype == TEXTOID) {
            val = CStringGetTextDatum(
                DatumGetCString(FunctionCall1(&state->flinfo, val))
            );
        }
    } else {
        HeapTuple temptup =
            state->tupmap ? execute_attr_map_tuple(htup, state->tupmap) : htup;

        val = heap_copy_tuple_as_datum(temptup, state->outdesc);
    }

    return val;
}

/*
 * Expand Tuple fields into an intermediate array. When nested in another
 * array, fields form one additional inner dimension
 */
static Datum
pgch__convert_record_array(pgch_convert_state* state, Datum val) {
    pgch_tuple* slot = (pgch_tuple*)DatumGetPointer(val);
    pgch_array* out  = (pgch_array*)palloc(sizeof(pgch_array));

    out->len        = slot->len;
    out->ndim       = 1;
    out->item_type  = state->item_type;
    out->array_type = state->outtype;
    out->datums     = (Datum*)palloc(sizeof(Datum) * slot->len);
    out->nulls      = (bool*)palloc(sizeof(bool) * slot->len);

    for (size_t i = 0; i < slot->len; i++) {
        pgch_convert_state* field = state->field_states[i];

        out->nulls[i]  = slot->nulls[i];
        out->datums[i] = field && !slot->nulls[i] ? field->func(field, slot->datums[i])
                                                  : slot->datums[i];
    }

    return PointerGetDatum(out);
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

            if (slot->nulls[i] ||
                !pgch__flatten_array(child, dims, level + 1, values, nulls, idx)) {
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
    /* Fields of an expanded Tuple fill one more dimension */
    if (elem->func == pgch__convert_record_array) {
        slot->ndim++;
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
        int dims[MAXDIM]  = {};
        int lbs[MAXDIM]   = {};
        size_t idx        = 0;
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
            Datum* flat     = palloc(sizeof(Datum) * total);
            bool* flatnulls = palloc0(sizeof(bool) * total);

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
pgch__convert_tuple_array(pgch_convert_state* state, Datum val) {
    return pgch__convert_array(state, pgch__convert_record_array(state, val));
}

static bool
pgch__record_axes(Oid outtype) {
    return outtype == BOXOID || outtype == CIRCLEOID || outtype == LINEOID;
}

/* Flatten Point and Float64 fields left to right, as the writer fills them */
static int
pgch__tuple_axes(const pgch_tuple* slot, double* axes, int cap, int at) {
    for (size_t i = 0; i < slot->len && at >= 0; i++) {
        if (slot->nulls[i]) {
            return -1;
        }
        switch (slot->types[i]) {
        case POINTOID: {
            Point* p = DatumGetPointP(slot->datums[i]);

            if (at + 2 > cap) {
                return -1;
            }
            axes[at++] = p->x;
            axes[at++] = p->y;
            break;
        }
        case FLOAT8OID:
            if (at + 1 > cap) {
                return -1;
            }
            axes[at++] = DatumGetFloat8(slot->datums[i]);
            break;
        case RECORDOID:
            at = pgch__tuple_axes(
                (pgch_tuple*)DatumGetPointer(slot->datums[i]), axes, cap, at
            );
            break;
        default:
            return -1;
        }
    }
    return at;
}

/* box, circle and line have no cast from the Tuple of coordinates they cross as */
static Datum
pgch__convert_axes(pgch_convert_state* state, Datum val) {
    pgch_tuple* slot = (pgch_tuple*)DatumGetPointer(val);
    double axes[4];
    int want = state->outtype == BOXOID ? 4 : 3;

    if (pgch__tuple_axes(slot, axes, want, 0) != want) {
        pgch_errorf(
            ERRCODE_DATATYPE_MISMATCH,
            "cannot return %s as %s",
            slot->ch_type_name ? slot->ch_type_name : "?",
            format_type_be(state->outtype)
        );
    }
    switch (state->outtype) {
    case BOXOID: {
        BOX* box = (BOX*)palloc(sizeof(BOX));

        box->high.x = axes[0];
        box->high.y = axes[1];
        box->low.x  = axes[2];
        box->low.y  = axes[3];
        return BoxPGetDatum(box);
    }
    case CIRCLEOID: {
        CIRCLE* circle = (CIRCLE*)palloc(sizeof(CIRCLE));

        circle->center.x = axes[0];
        circle->center.y = axes[1];
        circle->radius   = axes[2];
        return CirclePGetDatum(circle);
    }
    case LINEOID: {
        LINE* line = (LINE*)palloc(sizeof(LINE));

        line->A = axes[0];
        line->B = axes[1];
        line->C = axes[2];
        return LinePGetDatum(line);
    }
    }
    pg_unreachable();
}

/* lseg is two points, which no cast makes of the point list it crosses as */
static Datum
pgch__convert_lseg(pgch_convert_state* state, Datum val) {
    Point* pts;
    int stored, npts;

    if (state->intype == PATHOID) {
        PATH* path = DatumGetPathP(val);

        pts    = path->p;
        stored = path->npts;
        /* A closed path stands for the point list with its first point again */
        npts = stored + (path->closed ? 1 : 0);
    } else {
        POLYGON* poly = DatumGetPolygonP(val);

        pts    = poly->p;
        stored = npts = poly->npts;
    }
    if (npts != 2) {
        pgch_errorf(
            ERRCODE_DATATYPE_MISMATCH,
            "cannot return %d points as %s",
            npts,
            format_type_be(state->outtype)
        );
    }
    LSEG* lseg = (LSEG*)palloc(sizeof(LSEG));
    lseg->p[0] = pts[0];
    lseg->p[1] = pts[1 % stored];
    return LsegPGetDatum(lseg);
}

static Datum
pgch__convert_from_text(pgch_convert_state* state, Datum val) {
    return InputFunctionCall(
        &state->flinfo, TextDatumGetCString(val), state->typioparam, state->typmod
    );
}

/* PostgreSQL casts to a string type by rendering through the output function */
static Datum
pgch__convert_to_text(pgch_convert_state* state, Datum val) {
    char* str = OutputFunctionCall(&state->outflinfo, val);
    Datum result =
        InputFunctionCall(&state->flinfo, str, state->typioparam, state->typmod);

    pfree(str);
    return result;
}

/* ClickHouse UInt8 maps to smallint, convert requested boolean explicitly */
static Datum
pgch__convert_bool(pgch_convert_state* state pg_attribute_unused(), Datum val) {
    return BoolGetDatum(DatumGetInt16(val));
}

/*
 * DateTime is an instant while time carries no zone, so PostgreSQL casts one
 * to the other through the session zone. Take the UTC time of day instead:
 * a time goes out as an instant on the epoch day in UTC, and the value read
 * back must not depend on the session the reader happens to sit in.
 */
static Datum
pgch__convert_time(pgch_convert_state* state pg_attribute_unused(), Datum val) {
    TimeADT t = DatumGetTimestampTz(val) % USECS_PER_DAY;

    return TimeADTGetDatum(t < 0 ? t + USECS_PER_DAY : t);
}

/*
 * PostgreSQL cast lookup unwraps domains to their base types. Check domain
 * constraints here because no cast function will do so
 */
static Datum
pgch__convert_domain(pgch_convert_state* state, Datum val) {
    if (state->inner) {
        val = state->inner->func(state->inner, val);
    }
    domain_check(
        val, false, state->outtype, &state->domain_extra, GetMemoryChunkContext(state)
    );
    return val;
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

/*
 * Set up length or precision conversion omitted by PostgreSQL's cast path
 * This also handles values that do not need a cast
 */
static bool
pgch__init_typmod_coerce(pgch_convert_state* state) {
    Oid funcid;

    if (state->typmod < 0 ||
        find_typmod_coercion_function(state->outtype, &funcid) != COERCION_PATH_FUNC) {
        return false;
    }
    fmgr_info(funcid, &state->tmflinfo);
    return true;
}

/*
 * Return elem[] when Tuple should become array of items, or InvalidOid for record.
 * Anonymous records cannot define PostgreSQL column shape. Keep Tuple intact
 * for absent, composite, or geometric elem. Array fields also require record
 * conversion because fields still contain pgch_array intermediates
 */
static Oid
pgch__spread_type(const chc_type* ct, const pgch_tuple* slot, Oid elem) {
    size_t nfields = ct ? chc_type_n_children(ct) : slot->len;

    if (!OidIsValid(elem) || pgch__record_axes(elem) ||
        type_is_rowtype(getBaseType(elem))) {
        return InvalidOid;
    }
    for (size_t i = 0; i < nfields; i++) {
        Oid ftype = ct ? pgch_datum_oid(chc_type_child(ct, i)) : slot->types[i];

        if (ftype == ANYARRAYOID) {
            return InvalidOid;
        }
    }
    return get_array_type(elem);
}

/* PostgreSQL doesn't support coercion from numeric to its unsigned 64 bit types */
static inline bool
pgch__uint64_type(Oid typid) {
#if PG_VERSION_NUM >= 190000
    if (typid == OID8OID) {
        return true;
    }
#endif
    return typid == XID8OID;
}

static pgch_convert_state*
pgch__convert_init(
    const chc_type* ct,
    Datum val,
    Oid intype,
    Oid outtype,
    int32 outtypmod
) {
    /* ClickHouse Nothing and Void values are always NULL */
    if (!OidIsValid(intype)) {
        return NULL;
    }
    if (OidIsValid(outtype)) {
        int32 basetypmod = outtypmod;
        Oid base         = getBaseTypeAndTypmod(outtype, &basetypmod);

        if (base != outtype) {
            pgch_convert_state* dom = palloc0(sizeof(pgch_convert_state));

            dom->outtype = outtype;
            dom->func    = pgch__convert_domain;
            dom->inner   = pgch__convert_init(ct, val, intype, base, basetypmod);
            return dom;
        }
    }
    if (intype == TEXTOID && outtype == BYTEAOID) {
        return NULL;
    }

    pgch_convert_state* state = palloc0(sizeof(pgch_convert_state));

    if (ct) {
        ct = pgch_unwrap(ct, NULL);
    }
    state->intype  = intype;
    state->outtype = outtype;
    state->typmod  = outtypmod;
    state->ctype   = COERCION_PATH_NONE;

    if (intype == ANYARRAYOID) {
        pgch_array* slot     = ct ? NULL : (pgch_array*)DatumGetPointer(val);
        const chc_type* leaf = ct;
        /* Array domains expose element type and type modifier through base type */
        Oid out_base = OidIsValid(outtype) ? getBaseTypeAndTypmod(outtype, &outtypmod)
                                           : InvalidOid;
        Oid out_elem = OidIsValid(out_base) ? get_element_type(out_base) : InvalidOid;

        if (ct) {
            int ndim;

            state->item_type = pgch__array_item(ct, &leaf, &ndim);
            state->intype    = pgch_native_oid(ct);
        } else {
            state->item_type = slot->item_type;
            state->intype    = slot->array_type;
        }
        state->func = pgch__convert_array;

        /* PostgreSQL reports array casts separately from scalar element casts,
         * and its COERCION_PATH_ARRAYCOERCE applies a type modifier per element */
        if (OidIsValid(out_elem) && (out_elem != state->item_type || outtypmod >= 0)) {
            Datum leafval  = (Datum)0;
            bool have_leaf = ct || pgch__array_leaf(slot, &leafval);

            /* Records without a value to inspect are empty or all NULL, so
             * they need the element type named but no conversion built
             * Pass array column's type modifier to each element */
            if (have_leaf || state->item_type != RECORDOID) {
                Oid elem_out = out_elem;

                if (state->item_type == RECORDOID) {
                    Oid spread = pgch__spread_type(
                        leaf, (const pgch_tuple*)DatumGetPointer(leafval), out_elem
                    );

                    if (OidIsValid(spread)) {
                        elem_out = spread;
                    }
                }
                state->elem_state = pgch__convert_init(
                    leaf, leafval, state->item_type, elem_out, outtypmod
                );
                /* This array builds every dimension at once, so leave the
                 * fields of each Tuple unbuilt */
                if (state->elem_state &&
                    state->elem_state->func == pgch__convert_tuple_array) {
                    state->elem_state->func = pgch__convert_record_array;
                }
            }
            if (out_elem != state->item_type) {
                state->item_type = out_elem;
                state->intype    = outtype;
            }
        }
        get_typlenbyvalalign(
            state->item_type, &state->typlen, &state->typbyval, &state->typalign
        );
        intype = state->intype;
    }

    if (intype == RECORDOID && pgch__record_axes(outtype)) {
        state->func = pgch__convert_axes;
        return state;
    }
    if (outtype == LSEGOID && (intype == PATHOID || intype == POLYGONOID)) {
        state->func = pgch__convert_lseg;
        return state;
    }

    if (intype == RECORDOID) {
        pgch_tuple* slot = ct ? NULL : (pgch_tuple*)DatumGetPointer(val);
        size_t nfields   = ct ? chc_type_n_children(ct) : slot->len;
        Oid item         = get_element_type(outtype);

        if (OidIsValid(pgch__spread_type(ct, slot, item))) {
            state->func         = pgch__convert_tuple_array;
            state->item_type    = item;
            state->field_states = palloc(sizeof(void*) * nfields);
            get_typlenbyvalalign(
                item, &state->typlen, &state->typbyval, &state->typalign
            );

            for (size_t i = 0; i < nfields; ++i) {
                const chc_type* ft = ct ? chc_type_child(ct, i) : NULL;
                Oid ftype          = ct ? pgch_datum_oid(ft) : slot->types[i];
                Datum fval         = ct ? (Datum)0 : slot->datums[i];

                state->field_states[i] =
                    !ct && slot->nulls[i]
                        ? NULL
                        : pgch__convert_init(ft, fval, ftype, item, outtypmod);
            }
            return state;
        }

        state->func         = pgch__convert_record;
        state->indesc       = CreateTemplateTupleDesc(nfields);
        state->field_states = palloc(sizeof(void*) * nfields);

        if (outtype == TEXTOID) {
            fmgr_info(F_RECORD_OUT, &state->flinfo);
        } else if (outtype != RECORDOID) {
            TypeCacheEntry* typentry = lookup_type_cache(
                outtype, TYPECACHE_TUPDESC | TYPECACHE_DOMAIN_BASE_INFO
            );
            TupleDesc tupdesc;

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
            int32 item_typmod  = -1;

            if (ftype == ANYARRAYOID && !isnull) {
                item_type =
                    ct ? pgch_native_oid(ft)
                       : ((pgch_array*)DatumGetPointer(slot->datums[i]))->array_type;
            }
            if (state->outdesc && ftype == RECORDOID &&
                i < (size_t)state->outdesc->natts) {
                item_type = TupleDescAttr(state->outdesc, i)->atttypid;
            }
            /* Use target field's type modifier only when field types match */
            if (state->outdesc && i < (size_t)state->outdesc->natts &&
                item_type == TupleDescAttr(state->outdesc, i)->atttypid) {
                item_typmod = TupleDescAttr(state->outdesc, i)->atttypmod;
            }

            state->field_states[i] = isnull ? NULL
                                            : pgch__convert_init(
                                                  ft,
                                                  ct ? (Datum)0 : slot->datums[i],
                                                  ftype,
                                                  item_type,
                                                  item_typmod
                                              );

            TupleDescInitEntry(
                state->indesc, (AttrNumber)i + 1, "", item_type, item_typmod, 0
            );
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
        /* Array and record values arrive unbuilt, with their converter set */
        bool composite = state->func != NULL;

        if (!state->func) {
            state->func = pgch__convert_generic;
        }

        if (intype == TEXTOID) {
            /* Domain supplies its own type modifier */
            Oid baseTypeId = getBaseTypeAndTypmod(outtype, &state->typmod);
            Oid typinput;

            getTypeInputInfo(baseTypeId, &typinput, &state->typioparam);
            fmgr_info(typinput, &state->flinfo);
            state->func = pgch__convert_from_text;
        } else if (outtype == BOOLOID && intype == INT2OID) {
            state->func = pgch__convert_bool;
        } else if (outtype == TIMEOID && intype == TIMESTAMPTZOID) {
            state->func = pgch__convert_time;
        } else {
            Oid castfunc;

            state->ctype =
                find_coercion_pathway(outtype, intype, COERCION_EXPLICIT, &castfunc);
            switch (state->ctype) {
            case COERCION_PATH_FUNC:
                fmgr_info(castfunc, &state->flinfo);
                /* Cast function accepts target typmod, skip separate coercion */
                if (state->flinfo.fn_nargs > 1) {
                    return state;
                }
                break;
            case COERCION_PATH_NONE:
                if (intype != NUMERICOID || !pgch__uint64_type(outtype)) {
                    goto no_cast;
                }
                state->ctype = COERCION_PATH_COERCEVIAIO;
                /* FALLTHROUGH */
                CHC_FALLTHROUGH;
            case COERCION_PATH_COERCEVIAIO: {
                Oid typinput;
                Oid typoutput;
                bool varlena;

                if (composite) {
                    goto no_cast;
                }
                /* Domain supplies its own type modifier */
                Oid baseTypeId = getBaseTypeAndTypmod(outtype, &state->typmod);
                getTypeOutputInfo(intype, &typoutput, &varlena);
                fmgr_info(typoutput, &state->outflinfo);
                getTypeInputInfo(baseTypeId, &typinput, &state->typioparam);
                fmgr_info(typinput, &state->flinfo);
                state->func = pgch__convert_to_text;

                /* Input function already applied the type modifier */
                return state;
            }
            case COERCION_PATH_RELABELTYPE:
                if (state->func == NULL) {
                    goto no_conversion;
                }
                break;
            default:
            no_cast:
                pgch_errorf(
                    ERRCODE_FDW_INVALID_DATA_TYPE,
                    "could not cast value from %s to %s",
                    format_type_be(intype),
                    format_type_be(outtype)
                );
            }
            pgch__init_typmod_coerce(state);
        }
    } else if (!state->func) {
    no_conversion:
        /* Matching types may still need lower precision */
        if (!pgch__init_typmod_coerce(state)) {
            pfree(state);
            return NULL;
        }
        state->func = pgch__convert_generic;
    }

    return state;
}

void*
pgch_convert_init(Datum val, Oid intype, Oid outtype, int32 outtypmod) {
    return pgch__convert_init(NULL, val, intype, outtype, outtypmod);
}

void*
pgch_convert_init_type(const chc_type* in, Oid outtype, int32 outtypmod) {
    return pgch__convert_init(in, (Datum)0, pgch_datum_oid(in), outtype, outtypmod);
}

void*
pgch_reader_convert_init(
    const pgch_reader* r,
    size_t col,
    Oid outtype,
    int32 outtypmod
) {
    if (col >= r->ncols || r->cur == NULL) {
        return NULL;
    }
    return pgch__convert_init(
        chc_block_column_type(r->cur, col),
        (Datum)0,
        r->coltypes[col],
        outtype,
        outtypmod
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
        /* Anonymous records have no array to build, so render them as text */
        Oid array_type = slot->item_type == RECORDOID ? TEXTARRAYOID : slot->array_type;
        void* state    = pgch_convert_init(value, ANYARRAYOID, array_type, -1);
        Datum arr      = pgch_convert(state, value);

        getTypeOutputInfo(array_type, &out_func, &varlena);
        if (state) {
            pgch_convert_free(state);
        }
        return OidOutputFunctionCall(out_func, arr);
    }

    if (coltype == RECORDOID) {
        void* state = pgch_convert_init(value, RECORDOID, TEXTOID, -1);
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
