/*
 * Encode PostgreSQL Datums into ClickHouse Native blocks
 *
 * Define PGCH_IMPLEMENTATION in one translation unit. Include this header
 * without that definition everywhere else. Consumers provide transport
 */

#ifndef PG_CLICKHOUSE_ENCODE_H
#define PG_CLICKHOUSE_ENCODE_H

#include "pg-clickhouse.h"

#include "executor/tuptable.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Describe one output column, type must outlive writer */
typedef struct pgch_col {
    const char* name;
    size_t name_len;
    const chc_type* type;
} pgch_col;

typedef struct pgch_writer pgch_writer;

/*
 * Create writer in a child context of parent
 * Copy column names, borrow column types, reject unsupported types
 */
extern pgch_writer*
pgch_writer_new(MemoryContext parent, const pgch_col* cols, size_t ncols);

extern void
pgch_writer_free(pgch_writer* w);

/* ---- policies ------------------------------------------------------- */

/* Choose whether NULL arrays raise or become empty arrays */
typedef enum pgch_null_array {
    PGCH_NULL_ARRAY_ERROR = 0,
    PGCH_NULL_ARRAY_EMPTY,
} pgch_null_array;

/* Choose how writer handles NaN and Infinity */
typedef enum pgch_nonfinite {
    PGCH_NONFINITE_KEEP = 0,
    PGCH_NONFINITE_NULL,
    PGCH_NONFINITE_ZERO,
} pgch_nonfinite;

extern void
pgch_writer_set_null_array(pgch_writer* w, pgch_null_array policy);
extern void
pgch_writer_set_nonfinite(pgch_writer* w, pgch_nonfinite policy);

/*
 * Append PostgreSQL value to column
 * Pass value OID in valtype, or ANYARRAYOID for pgch_array
 * Append exactly one value to every column in each row
 */
extern void
pgch_append_datum(pgch_writer* w, size_t col, Datum val, Oid valtype, bool isnull);

/*
 * Append one slot
 * Skip dropped and generated attributes like pgch_structure_from_tupdesc
 */
extern void
pgch_append_slot(pgch_writer* w, TupleTableSlot* slot);

/*
 * Convert PostgreSQL array into pgch_array
 * Pass returned Datum to pgch_append_datum with ANYARRAYOID
 */
extern Datum
pgch_array_from_pg(Datum arr, Oid elemtype, int16 typlen, bool typbyval, char typalign);

/* ---- typed appends -------------------------------------------------- */

/*
 * Append typed C values
 * Set isnull for NULL, destination must accept NULL
 * Open array context makes each append target current element column
 */
extern void
pgch_append_int(pgch_writer* w, size_t col, int64_t val, bool isnull);
extern void
pgch_append_uint(pgch_writer* w, size_t col, uint64_t val, bool isnull);
extern void
pgch_append_bool(pgch_writer* w, size_t col, bool val, bool isnull);
extern void
pgch_append_double(pgch_writer* w, size_t col, double val, bool isnull);
extern void
pgch_append_float(pgch_writer* w, size_t col, float val, bool isnull);
extern void
pgch_append_bytes(pgch_writer* w, size_t col, const void* p, size_t n, bool isnull);

/* Append decimal text in `[-]digits[.frac]` form */
extern void
pgch_append_decimal(pgch_writer* w, size_t col, const char* digits, bool isnull);

extern void
pgch_append_uuid(pgch_writer* w, size_t col, const uint8_t bytes[16], bool isnull);

/* Append 4-byte IPv4 or 16-byte IPv6 address in network order */
extern void
pgch_append_inet(
    pgch_writer* w,
    size_t col,
    const uint8_t* addr_be,
    size_t addrlen,
    bool isnull
);

/*
 * Pass Date and DateTime as Unix seconds, Time as seconds since midnight
 * Pass DateTime64 and Time64 as integers scaled to column precision
 */
extern void
pgch_append_date_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull);
extern void
pgch_append_datetime_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull);
extern void
pgch_append_datetime64_raw(pgch_writer* w, size_t col, int64_t raw, bool isnull);
extern void
pgch_append_time_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull);
extern void
pgch_append_time64_raw(pgch_writer* w, size_t col, int64_t raw, bool isnull);

/*
 * Open array element context
 * Nest calls for nested arrays, close each call with pgch_array_end
 */
extern void
pgch_array_begin(pgch_writer* w, size_t col);
extern void
pgch_array_end(pgch_writer* w);
extern bool
pgch_array_active(const pgch_writer* w);

/* Return current ClickHouse kind, including active array element kind */
extern chc_kind
pgch_column_kind(const pgch_writer* w, size_t col);

/* Return DateTime64 or Time64 scale, zero for other types */
extern uint32_t
pgch_column_datetime64_scale(const pgch_writer* w, size_t col);

/* ---- block assembly ------------------------------------------------- */

/* Return buffered row count */
extern size_t
pgch_writer_rows(const pgch_writer* w);

/* Return bytes buffered across all columns */
extern size_t
pgch_writer_bytes(const pgch_writer* w);

/*
 * Build block over buffered rows
 * Close all array contexts first, use result before pgch_writer_reset
 */
extern const chc_block_builder*
pgch_writer_build(pgch_writer* w);

/* Release built block and reuse empty buffers */
extern void
pgch_writer_reset(pgch_writer* w);

/*
 * Append serialized block to out, then reset writer
 * Pass NULL opts to use pgch_block_opts_local
 */
extern void
pgch_writer_flush(pgch_writer* w, pgch_buf* out, const chc_block_opts* opts);

#ifdef PGCH_IMPLEMENTATION

#include <math.h>
#include <string.h>
#include <sys/socket.h> /* PostgreSQL inet macros require AF_INET */

#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "fmgr.h"
#include "parser/parse_coerce.h"
#include "port/pg_bswap.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#if PG_VERSION_NUM >= 190000
#include "varatt.h"
#endif

static inline void
pgch__offs_push(pgch_buf* b, uint64_t v) {
    pgch_buf_append(b, &v, sizeof v);
}

static inline uint64_t*
pgch__offs_data(const pgch_buf* b) {
    return (uint64_t*)b->data;
}

static inline size_t
pgch__offs_len(const pgch_buf* b) {
    return b->len / sizeof(uint64_t);
}

typedef enum {
    PGCH__FIXED,
    PGCH__STRING,
    PGCH__NULLABLE,
    PGCH__ARRAY,
    PGCH__LC,
} pgch__node_kind;

typedef struct pgch__node pgch__node;

typedef struct pgch__cast {
    Oid from;
    Oid to;
    bool relabel;
    bool viaio;
    FmgrInfo flinfo;
    FmgrInfo infn;
    Oid ioparam;
} pgch__cast;

/* Cache element metadata of last PostgreSQL array type appended to a column */
typedef struct pgch__arrmeta {
    Oid valtype;
    ArrayMetaState meta;
} pgch__arrmeta;

struct pgch__node {
    pgch__node_kind kind;
    const chc_type* type;
    Oid target;
    pgch__cast* cast;
    pgch__arrmeta* arrmeta;
    union {
        struct {
            pgch_buf data;
            size_t elem_size;
            uint32_t dt64_scale;
        } fixed;

        struct {
            pgch_buf data;
            pgch_buf offs;
            bool is_json;
        } str;

        struct {
            pgch_buf null_map;
            pgch__node* inner;
        } nullable;

        struct {
            pgch_buf offs;
            pgch__node* values;
        } array;

        struct {
            pgch_buf data;
            pgch_buf offs;
            pgch_buf null_map;
            bool inner_nullable;
        } lc;
    };
};

typedef struct pgch__col {
    const char* name;
    size_t name_len;
    const chc_type* t;
    pgch__node* root;
} pgch__col;

struct pgch_writer {
    MemoryContext cxt;
    MemoryContext bcxt;

    size_t ncols;
    pgch__col* cols;

    pgch_null_array null_array;
    pgch_nonfinite nonfinite;

    chc_block_builder bb;

    pgch__node** cursor;
    size_t cursor_len;
    size_t cursor_cap;
    size_t cursor_col;
};

static pgch__node*
pgch__node_new(const chc_type* t) {
    pgch__node* n = palloc0(sizeof(pgch__node));

    n->type = t;
    switch (chc_type_kind(t)) {
    case CHC_NULLABLE:
        n->kind           = PGCH__NULLABLE;
        n->nullable.inner = pgch__node_new(chc_type_child(t, 0));
        return n;
    case CHC_ARRAY:
        n->kind         = PGCH__ARRAY;
        n->array.values = pgch__node_new(chc_type_child(t, 0));
        return n;
    case CHC_LOW_CARDINALITY: {
        /* ClickHouse nests Nullable inside LowCardinality */
        const chc_type* inner = chc_type_child(t, 0);
        bool inner_nullable   = chc_type_kind(inner) == CHC_NULLABLE;
        const chc_type* base  = inner_nullable ? chc_type_child(inner, 0) : inner;

        if (chc_type_kind(base) != CHC_STRING) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE,
                "unsupported LowCardinality variant: %s",
                chc_type_name(base, NULL)
            );
        }
        n->kind              = PGCH__LC;
        n->lc.inner_nullable = inner_nullable;
        return n;
    }
    case CHC_STRING:
        n->kind = PGCH__STRING;
        return n;
    case CHC_JSON:
        n->kind        = PGCH__STRING;
        n->str.is_json = true;
        return n;
    case CHC_DATETIME64:
    case CHC_TIME64: {
        int scale = chc_type_datetime64_scale(t);

        if (scale < 0 || (size_t)scale >= lengthof(pgch_pow10)) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE,
                "%s scale %d out of range",
                chc_type_name(t, NULL),
                scale
            );
        }
        n->kind             = PGCH__FIXED;
        n->fixed.elem_size  = chc_type_elem_size(t);
        n->fixed.dt64_scale = (uint32_t)scale;
        return n;
    }
    default: {
        size_t es = chc_type_elem_size(t);

        if (es == 0) {
            pgch_errorf(
                ERRCODE_FDW_INVALID_DATA_TYPE,
                "unsupported column type: %s",
                chc_type_name(t, NULL)
            );
        }
        n->kind            = PGCH__FIXED;
        n->fixed.elem_size = es;
        return n;
    }
    }
}

pgch_writer*
pgch_writer_new(MemoryContext parent, const pgch_col* cols, size_t ncols) {
    MemoryContext cxt =
        AllocSetContextCreate(parent, "pgch writer", ALLOCSET_DEFAULT_SIZES);
    MemoryContext old = MemoryContextSwitchTo(cxt);
    pgch_writer* w;

    PG_TRY();
    {
        w        = palloc0(sizeof(*w));
        w->cxt   = cxt;
        w->ncols = ncols;
        w->cols  = ncols ? palloc0(ncols * sizeof(pgch__col)) : NULL;

        for (size_t i = 0; i < ncols; i++) {
            pgch__col* c = &w->cols[i];

            c->name     = pnstrdup(cols[i].name ? cols[i].name : "", cols[i].name_len);
            c->name_len = cols[i].name_len;
            c->t        = cols[i].type;
            c->root     = pgch__node_new(cols[i].type);
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    return w;
}

void
pgch_writer_free(pgch_writer* w) {
    if (w) {
        MemoryContextDelete(w->cxt);
    }
}

void
pgch_writer_set_null_array(pgch_writer* w, pgch_null_array policy) {
    w->null_array = policy;
}

void
pgch_writer_set_nonfinite(pgch_writer* w, pgch_nonfinite policy) {
    w->nonfinite = policy;
}

static inline pgch__node*
pgch__cursor_node(const pgch_writer* w, size_t col) {
    return w->cursor_len ? w->cursor[w->cursor_len - 1]->array.values
                         : w->cols[col].root;
}

static inline size_t
pgch__target_col(const pgch_writer* w, size_t col) {
    return w->cursor_len ? w->cursor_col : col;
}

static const char*
pgch__col_desc(const pgch_writer* w, size_t col) {
    size_t i = pgch__target_col(w, col);

    if (i < w->ncols && w->cols[i].name[0]) {
        return psprintf("column \"%s\"", w->cols[i].name);
    }
    return psprintf("column %zu", i);
}

static pgch__node*
pgch__resolve_leaf(pgch_writer* w, size_t col, bool isnull) {
    pgch__node* node;
    uint8_t b     = isnull ? 1 : 0;
    bool nullable = false;

    if (!w->cursor_len && col >= w->ncols) {
        pgch_errorf(ERRCODE_FDW_ERROR, "column %zu out of range", col);
    }
    node = pgch__cursor_node(w, col);

    while (node->kind == PGCH__NULLABLE) {
        pgch_buf_append(&node->nullable.null_map, &b, 1);
        nullable = true;
        node     = node->nullable.inner;
    }
    if (node->kind == PGCH__LC && node->lc.inner_nullable) {
        pgch_buf_append(&node->lc.null_map, &b, 1);
        nullable = true;
    }
    if (isnull && !nullable) {
        const chc_type* t = w->cols[pgch__target_col(w, col)].t;
        size_t tnlen;
        const char* tname = chc_type_name(t, &tnlen);

        pgch_errorf(
            ERRCODE_NOT_NULL_VIOLATION,
            "cannot append NULL to NOT NULL %.*s %s",
            (int)tnlen,
            tname ? tname : "?",
            pgch__col_desc(w, col)
        );
    }
    if (node->kind == PGCH__ARRAY) {
        pgch_errorf(
            ERRCODE_DATATYPE_MISMATCH,
            "scalar value into Array %s",
            pgch__col_desc(w, col)
        );
    }
    return node;
}

static pgch_buf*
pgch__fixed_data(pgch__node* node) {
    if (node->kind != PGCH__FIXED) {
        pgch_error(
            ERRCODE_DATATYPE_MISMATCH, "fixed-width value into non-fixed-width column"
        );
    }
    return &node->fixed.data;
}

static void
pgch__append_row_offs(pgch_buf* data, pgch_buf* offs, const void* p, size_t n) {
    if (n) {
        pgch_buf_append(data, p, n);
    }
    pgch__offs_push(offs, data->len);
}

/* ClickHouse stores Decimal as a scaled, little-endian signed integer */
static void
pgch__decimal_to_bytes(const char* s, uint32_t scale, size_t width, uint8_t* out) {
    const char* input = s;
    bool neg          = false;

    if (!s) {
        pgch_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "decimal parse failure");
    }
    if (*s == '-') {
        neg = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    const char* dot  = strchr(s, '.');
    size_t ilen      = dot ? (size_t)(dot - s) : strlen(s);
    const char* frac = dot ? dot + 1 : "";
    size_t flen      = strlen(frac);
    size_t ndig      = ilen + scale;

    uint32_t mag[8] = {};
    size_t nwords   = width / 4;
    bool spilled    = false;

    for (size_t i = 0; i < ndig; i++) {
        char c = i < ilen ? s[i] : i - ilen < flen ? frac[i - ilen] : '0';

        if (c < '0' || c > '9') {
            pgch_errorf(
                ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
                "cannot encode \"%s\" as ClickHouse Decimal",
                input
            );
        }
        uint64_t carry = (uint64_t)(c - '0');

        for (size_t b = 0; b < nwords; b++) {
            uint64_t v = (uint64_t)mag[b] * 10 + carry;

            mag[b] = (uint32_t)v;
            carry  = v >> 32;
        }
        spilled |= carry != 0;
    }
    /* Sign bit set means magnitude no longer fits the signed width */
    if (spilled || (mag[nwords - 1] & 0x80000000u)) {
        pgch_errorf(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "value \"%s\" out of range for ClickHouse Decimal%zu",
            input,
            width * 8
        );
    }
    if (neg) {
        for (size_t b = 0; b < nwords; b++) {
            mag[b] = ~mag[b];
        }
        uint64_t carry = 1;

        for (size_t b = 0; b < nwords && carry; b++) {
            uint64_t v = (uint64_t)mag[b] + carry;

            mag[b] = (uint32_t)v;
            carry  = v >> 32;
        }
    }

    memcpy(out, mag, width);
}

static void
pgch__append_int_kind(pgch__node* node, int64_t val) {
    switch (chc_type_kind(node->type)) {
    case CHC_INT8:
    case CHC_UINT8:
    case CHC_BOOL: {
        int8_t v = (int8_t)val;

        pgch_buf_append(pgch__fixed_data(node), &v, 1);
        return;
    }
    case CHC_INT16:
    case CHC_UINT16: {
        int16_t v = (int16_t)val;

        pgch_buf_append(pgch__fixed_data(node), &v, 2);
        return;
    }
    case CHC_INT32:
    case CHC_UINT32: {
        int32_t v = (int32_t)val;

        pgch_buf_append(pgch__fixed_data(node), &v, 4);
        return;
    }
    case CHC_INT64:
    case CHC_UINT64:
        pgch_buf_append(pgch__fixed_data(node), &val, 8);
        return;
    default:
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "int value into non-integer column");
    }
}

void
pgch_append_int(pgch_writer* w, size_t col, int64_t val, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);

    if (isnull) {
        pgch_buf_append_zero(pgch__fixed_data(node), node->fixed.elem_size);
    } else {
        pgch__append_int_kind(node, val);
    }
    MemoryContextSwitchTo(old);
}

void
pgch_append_uint(pgch_writer* w, size_t col, uint64_t val, bool isnull) {
    pgch_append_int(w, col, (int64_t)val, isnull);
}

void
pgch_append_bool(pgch_writer* w, size_t col, bool val, bool isnull) {
    pgch_append_int(w, col, val, isnull);
}

void
pgch_append_double(pgch_writer* w, size_t col, double val, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node;

    if (!isnull && !isfinite(val) && w->nonfinite != PGCH_NONFINITE_KEEP) {
        isnull = w->nonfinite == PGCH_NONFINITE_NULL;
        val    = 0;
    }
    node = pgch__resolve_leaf(w, col, isnull);
    if (isnull) {
        pgch_buf_append_zero(pgch__fixed_data(node), 8);
    } else {
        pgch_buf_append(pgch__fixed_data(node), &val, 8);
    }
    MemoryContextSwitchTo(old);
}

void
pgch_append_float(pgch_writer* w, size_t col, float val, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node;

    if (!isnull && !isfinite(val) && w->nonfinite != PGCH_NONFINITE_KEEP) {
        isnull = w->nonfinite == PGCH_NONFINITE_NULL;
        val    = 0;
    }
    node = pgch__resolve_leaf(w, col, isnull);
    if (isnull) {
        pgch_buf_append_zero(pgch__fixed_data(node), 4);
    } else {
        pgch_buf_append(pgch__fixed_data(node), &val, 4);
    }
    MemoryContextSwitchTo(old);
}

static void
pgch__append_bytes_fixed(pgch__node* node, const void* p, size_t n, bool isnull) {
    chc_kind k     = chc_type_kind(node->type);
    pgch_buf* data = &node->fixed.data;

    if (k == CHC_FIXED_STRING) {
        size_t width = node->fixed.elem_size;

        if (isnull) {
            pgch_buf_append_zero(data, width);
            return;
        }
        size_t take = n < width ? n : width;

        if (take) {
            pgch_buf_append(data, p, take);
        }
        if (take < width) {
            pgch_buf_append_zero(data, width - take);
        }
        return;
    }
    if (k == CHC_ENUM8 || k == CHC_ENUM16) {
        if (isnull) {
            pgch_buf_append_zero(data, node->fixed.elem_size);
            return;
        }
        size_t nenum = chc_type_enum_count(node->type);
        int64_t val  = 0;
        bool found   = false;

        for (size_t i = 0; i < nenum; i++) {
            const char* en;
            size_t el;
            int64_t ev;

            chc_type_enum_at(node->type, i, &en, &el, &ev);
            if (el == n && memcmp(en, p, n) == 0) {
                val   = ev;
                found = true;
                break;
            }
        }
        if (!found) {
            pgch_errorf(
                ERRCODE_INVALID_TEXT_REPRESENTATION,
                "enum value '%.*s' not found",
                (int)n,
                (const char*)p
            );
        }
        if (k == CHC_ENUM8) {
            int8_t v = (int8_t)val;

            pgch_buf_append(data, &v, 1);
        } else {
            int16_t v = (int16_t)val;

            pgch_buf_append(data, &v, 2);
        }
        return;
    }
    pgch_error(ERRCODE_DATATYPE_MISMATCH, "bytes into non-text column");
}

void
pgch_append_bytes(pgch_writer* w, size_t col, const void* p, size_t n, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);

    switch (node->kind) {
    case PGCH__LC:
        pgch__append_row_offs(
            &node->lc.data, &node->lc.offs, isnull ? NULL : p, isnull ? 0 : n
        );
        break;
    case PGCH__STRING:
        if (isnull && node->str.is_json) {
            /* ClickHouse validates JSON values even when null map marks NULL */
            pgch__append_row_offs(&node->str.data, &node->str.offs, "{}", 2);
        } else {
            pgch__append_row_offs(
                &node->str.data, &node->str.offs, isnull ? NULL : p, isnull ? 0 : n
            );
        }
        break;
    case PGCH__FIXED:
        pgch__append_bytes_fixed(node, p, n, isnull);
        break;
    default:
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "bytes into non-text column");
    }
    MemoryContextSwitchTo(old);
}

/* PostgreSQL added numeric Infinity in version 14 */
static inline bool
pgch__numeric_nonfinite(Numeric num) {
#if PG_VERSION_NUM >= 140000
    return numeric_is_nan(num) || numeric_is_inf(num);
#else
    return numeric_is_nan(num);
#endif
}

/* PostgreSQL numeric text renders non-finite values as words */
static bool
pgch__finite_decimal(const char* s) {
    if (*s == '-' || *s == '+') {
        s++;
    }
    return *s >= '0' && *s <= '9';
}

void
pgch_append_decimal(pgch_writer* w, size_t col, const char* digits, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node;
    size_t width;

    if (!isnull && digits && w->nonfinite != PGCH_NONFINITE_KEEP &&
        !pgch__finite_decimal(digits)) {
        isnull = w->nonfinite == PGCH_NONFINITE_NULL;
        digits = NULL;
    }
    node = pgch__resolve_leaf(w, col, isnull);

    switch (chc_type_kind(node->type)) {
    case CHC_DECIMAL32:
        width = 4;
        break;
    case CHC_DECIMAL64:
        width = 8;
        break;
    case CHC_DECIMAL128:
        width = 16;
        break;
    case CHC_DECIMAL256:
        width = 32;
        break;
    default:
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "decimal into non-decimal column");
    }
    uint8_t raw[32] = {};

    if (!isnull && digits) {
        uint32_t scale = (uint32_t)chc_type_decimal_scale(node->type);

        pgch__decimal_to_bytes(digits, scale, width, raw);
    }
    pgch_buf_append(pgch__fixed_data(node), raw, width);
    MemoryContextSwitchTo(old);
}

void
pgch_append_uuid(pgch_writer* w, size_t col, const uint8_t bytes[16], bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    uint8_t wire[16]  = {};

    if (!isnull) {
        uint64_t a, b;

        memcpy(&a, bytes, 8);
        memcpy(&b, bytes + 8, 8);
        a = pg_ntoh64(a);
        b = pg_ntoh64(b);
        memcpy(wire, &a, 8);
        memcpy(wire + 8, &b, 8);
    }
    pgch_buf_append(pgch__fixed_data(node), wire, 16);
    MemoryContextSwitchTo(old);
}

/* ClickHouse stores IPv4 in host order and IPv6 in network order */
void
pgch_append_inet(
    pgch_writer* w,
    size_t col,
    const uint8_t* addr_be,
    size_t addrlen,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    chc_kind k        = chc_type_kind(node->type);

    if (k == CHC_IPV4 && addrlen == 4) {
        uint32_t addr = 0;

        if (!isnull && addr_be) {
            uint32_t be;

            memcpy(&be, addr_be, 4);
            addr = pg_ntoh32(be);
        }
        pgch_buf_append(pgch__fixed_data(node), &addr, 4);
        MemoryContextSwitchTo(old);
        return;
    }
    if (k == CHC_IPV6 && addrlen == 16) {
        uint8_t raw[16] = {};

        if (!isnull && addr_be) {
            memcpy(raw, addr_be, 16);
        }
        pgch_buf_append(pgch__fixed_data(node), raw, 16);
        MemoryContextSwitchTo(old);
        return;
    }
    pgch_error(ERRCODE_DATATYPE_MISMATCH, "cannot insert inet into non-inet column");
}

void
pgch_append_date_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    chc_kind k        = chc_type_kind(node->type);

    if (k == CHC_DATE) {
        uint16_t days = isnull ? 0 : (uint16_t)(seconds / SECS_PER_DAY);

        pgch_buf_append(pgch__fixed_data(node), &days, 2);
    } else if (k == CHC_DATE32) {
        int32_t days = isnull ? 0 : (int32_t)(seconds / SECS_PER_DAY);

        pgch_buf_append(pgch__fixed_data(node), &days, 4);
    } else {
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "date into non-date column");
    }
    MemoryContextSwitchTo(old);
}

void
pgch_append_datetime_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    uint32_t v        = isnull ? 0 : (uint32_t)seconds;

    pgch_buf_append(pgch__fixed_data(node), &v, 4);
    MemoryContextSwitchTo(old);
}

void
pgch_append_datetime64_raw(pgch_writer* w, size_t col, int64_t raw, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    int64_t v         = isnull ? 0 : raw;

    pgch_buf_append(pgch__fixed_data(node), &v, 8);
    MemoryContextSwitchTo(old);
}

/* ClickHouse Time supports signed values up to 999 hours */
void
pgch_append_time_seconds(pgch_writer* w, size_t col, int64_t seconds, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    int32_t v         = isnull ? 0 : (int32_t)seconds;

    if (chc_type_kind(node->type) != CHC_TIME) {
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "time into non-Time column");
    }
    pgch_buf_append(pgch__fixed_data(node), &v, 4);
    MemoryContextSwitchTo(old);
}

void
pgch_append_time64_raw(pgch_writer* w, size_t col, int64_t raw, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    int64_t v         = isnull ? 0 : raw;

    if (chc_type_kind(node->type) != CHC_TIME64) {
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "time into non-Time64 column");
    }
    pgch_buf_append(pgch__fixed_data(node), &v, 8);
    MemoryContextSwitchTo(old);
}

static uint64_t
pgch__node_rows(const pgch__node* n) {
    switch (n->kind) {
    case PGCH__FIXED:
        return n->fixed.elem_size ? n->fixed.data.len / n->fixed.elem_size : 0;
    case PGCH__STRING:
        return pgch__offs_len(&n->str.offs);
    case PGCH__NULLABLE:
        return pgch__node_rows(n->nullable.inner);
    case PGCH__ARRAY:
        return pgch__offs_len(&n->array.offs);
    case PGCH__LC:
        return pgch__offs_len(&n->lc.offs);
    }
    pg_unreachable();
}

void
pgch_array_begin(pgch_writer* w, size_t col) {
    pgch__node* node;

    if (w->cursor_len) {
        node = w->cursor[w->cursor_len - 1]->array.values;
    } else {
        if (col >= w->ncols) {
            pgch_errorf(ERRCODE_FDW_ERROR, "array_begin: column %zu out of range", col);
        }
        node = w->cols[col].root;
    }

    MemoryContext old = MemoryContextSwitchTo(w->cxt);

    if (node->kind == PGCH__NULLABLE && node->nullable.inner->kind == PGCH__ARRAY) {
        uint8_t b = 0;

        pgch_buf_append(&node->nullable.null_map, &b, 1);
        node = node->nullable.inner;
    }
    if (node->kind != PGCH__ARRAY) {
        pgch_error(ERRCODE_FDW_ERROR, "array_begin: column is not Array");
    }
    if (w->cursor_len == w->cursor_cap) {
        w->cursor_cap = w->cursor_cap ? w->cursor_cap * 2 : 4;
        w->cursor = w->cursor ? repalloc(w->cursor, w->cursor_cap * sizeof(pgch__node*))
                              : palloc(w->cursor_cap * sizeof(pgch__node*));
    }
    if (w->cursor_len == 0) {
        w->cursor_col = col;
    }
    w->cursor[w->cursor_len++] = node;
    MemoryContextSwitchTo(old);
}

void
pgch_array_end(pgch_writer* w) {
    if (w->cursor_len == 0) {
        return;
    }
    pgch__node* a     = w->cursor[--w->cursor_len];
    MemoryContext old = MemoryContextSwitchTo(w->cxt);

    pgch__offs_push(&a->array.offs, pgch__node_rows(a->array.values));
    MemoryContextSwitchTo(old);
}

bool
pgch_array_active(const pgch_writer* w) {
    return w && w->cursor_len > 0;
}

chc_kind
pgch_column_kind(const pgch_writer* w, size_t col) {
    if (!w->cursor_len && col >= w->ncols) {
        return CHC_VOID;
    }

    const pgch__node* node = pgch__cursor_node(w, col);

    if (node->kind == PGCH__NULLABLE) {
        node = node->nullable.inner;
    }
    switch (node->kind) {
    case PGCH__ARRAY:
        return CHC_ARRAY;
    case PGCH__LC:
        return CHC_STRING;
    default:
        return chc_type_kind(node->type);
    }
}

uint32_t
pgch_column_datetime64_scale(const pgch_writer* w, size_t col) {
    if (!w->cursor_len && col >= w->ncols) {
        return 0;
    }
    const pgch__node* node = pgch__cursor_node(w, col);

    for (;;) {
        if (node->kind == PGCH__NULLABLE) {
            node = node->nullable.inner;
        } else if (node->kind == PGCH__ARRAY) {
            node = node->array.values;
        } else {
            break;
        }
    }
    return node->kind == PGCH__FIXED ? node->fixed.dt64_scale : 0;
}

static void
pgch__append_null_array(pgch_writer* w, size_t col) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__cursor_node(w, col);
    uint8_t b         = 1;
    bool nullable     = w->null_array == PGCH_NULL_ARRAY_EMPTY;

    while (node->kind == PGCH__NULLABLE) {
        pgch_buf_append(&node->nullable.null_map, &b, 1);
        nullable = true;
        node     = node->nullable.inner;
    }
    if (!nullable || node->kind != PGCH__ARRAY) {
        const chc_type* t = w->cols[w->cursor_len ? w->cursor_col : col].t;
        size_t tnlen;
        const char* tname = chc_type_name(t, &tnlen);

        pgch_errorf(
            ERRCODE_NOT_NULL_VIOLATION,
            "cannot append NULL to NOT NULL %.*s %s",
            (int)tnlen,
            tname ? tname : "?",
            pgch__col_desc(w, col)
        );
    }
    pgch__offs_push(&node->array.offs, pgch__node_rows(node->array.values));
    MemoryContextSwitchTo(old);
}

/* ---- Datum appends -------------------------------------------------- */

static pgch_array*
pgch__nest_array(
    int level,
    int ndim,
    int* dims,
    Oid item_type,
    Datum* flat,
    bool* flatnulls,
    size_t* idx
) {
    pgch_array* arr = palloc(sizeof(pgch_array));

    arr->len        = dims[level];
    arr->ndim       = ndim - level;
    arr->item_type  = item_type;
    arr->array_type = InvalidOid;
    arr->datums     = palloc(sizeof(Datum) * arr->len);
    arr->nulls      = palloc0(sizeof(bool) * arr->len);

    if (level + 1 == ndim) {
        for (size_t i = 0; i < arr->len; i++) {
            arr->datums[i] = flat[*idx];
            arr->nulls[i]  = flatnulls[*idx];
            (*idx)++;
        }
    } else {
        for (size_t i = 0; i < arr->len; i++) {
            pgch_array* child = pgch__nest_array(
                level + 1, ndim, dims, item_type, flat, flatnulls, idx
            );

            arr->datums[i] = PointerGetDatum(child);
        }
    }
    return arr;
}

Datum
pgch_array_from_pg(
    Datum arr,
    Oid elemtype,
    int16 typlen,
    bool typbyval,
    char typalign
) {
    AnyArrayType* v = DatumGetAnyArrayP(arr);
    int ndim        = AARR_NDIM(v);
    int* dims       = AARR_DIMS(v);
    size_t total    = ArrayGetNItems(ndim, dims);
    array_iter iter;
    pgch_array* out;

    if (ndim > MAXDIM) {
        pgch_errorf(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            "array depth %d exceeds maximum %d",
            ndim,
            MAXDIM
        );
    }

#if PG_VERSION_NUM < 190000
#define PGCH__ITER_SETUP() array_iter_setup(&iter, v)
#define PGCH__ITER_NEXT(nullp, j)                                                      \
    array_iter_next(&iter, (nullp), (j), typlen, typbyval, typalign)
#else
#define PGCH__ITER_SETUP() array_iter_setup(&iter, v, typlen, typbyval, typalign)
#define PGCH__ITER_NEXT(nullp, j) array_iter_next(&iter, (nullp), (j))
#endif

    if (ndim <= 1) {
        out             = palloc(sizeof(pgch_array));
        out->len        = total;
        out->ndim       = 1;
        out->item_type  = elemtype;
        out->array_type = InvalidOid;
        out->datums     = total ? palloc(sizeof(Datum) * total) : NULL;
        out->nulls      = total ? palloc(sizeof(bool) * total) : NULL;

        PGCH__ITER_SETUP();
        for (size_t j = 0; j < total; j++) {
            out->datums[j] = PGCH__ITER_NEXT(&out->nulls[j], j);
        }
    } else {
        Datum* flat     = palloc(sizeof(Datum) * total);
        bool* flatnulls = palloc0(sizeof(bool) * total);
        size_t idx      = 0;

        PGCH__ITER_SETUP();
        for (size_t j = 0; j < total; j++) {
            flat[j] = PGCH__ITER_NEXT(&flatnulls[j], j);
        }
        out = pgch__nest_array(0, ndim, dims, elemtype, flat, flatnulls, &idx);

        pfree(flat);
        pfree(flatnulls);
    }

#undef PGCH__ITER_SETUP
#undef PGCH__ITER_NEXT

    return PointerGetDatum(out);
}

static bool
pgch__cast_value(
    pgch_writer* w,
    pgch__node* node,
    Datum val,
    Oid from,
    Oid to,
    Datum* out
) {
    pgch__cast* cast = node->cast;

    if (!cast || cast->from != from || cast->to != to) {
        Oid funcid;
        CoercionPathType path =
            find_coercion_pathway(to, from, COERCION_EXPLICIT, &funcid);

        if (!cast) {
            cast = node->cast = MemoryContextAllocZero(w->cxt, sizeof(*cast));
        }
        cast->relabel = false;
        cast->viaio   = false;

        switch (path) {
        case COERCION_PATH_FUNC:
            fmgr_info_cxt(funcid, &cast->flinfo, w->cxt);
            break;
        case COERCION_PATH_RELABELTYPE:
            cast->relabel = true;
            break;
        case COERCION_PATH_COERCEVIAIO: {
            Oid outfunc, infunc;
            bool varlena;

            /* PostgreSQL also offers I/O coercion from text to non-text types */
            if (to != TEXTOID) {
                cast->from = InvalidOid;
                return false;
            }
            getTypeOutputInfo(from, &outfunc, &varlena);
            getTypeInputInfo(to, &infunc, &cast->ioparam);
            fmgr_info_cxt(outfunc, &cast->flinfo, w->cxt);
            fmgr_info_cxt(infunc, &cast->infn, w->cxt);
            cast->viaio = true;
            break;
        }
        default:
            cast->from = InvalidOid;
            return false;
        }
        cast->from = from;
        cast->to   = to;
    }

    if (cast->relabel) {
        *out = val;
    } else if (cast->viaio) {
        char* s = OutputFunctionCall(&cast->flinfo, val);

        *out = InputFunctionCall(&cast->infn, s, cast->ioparam, -1);
        pfree(s);
    } else {
        *out = FunctionCall1(&cast->flinfo, val);
    }
    return true;
}

static void
pgch__append_one(
    pgch_writer* w,
    size_t col,
    chc_kind kind,
    Datum val,
    Oid valtype,
    bool isnull
) {
    if (kind == CHC_ARRAY) {
        if (isnull) {
            pgch__append_null_array(w, col);
            return;
        }
        if (valtype != ANYARRAYOID) {
            pgch__node* node  = pgch__cursor_node(w, col);
            pgch__arrmeta* am = node->arrmeta;
            ArrayMetaState* ms;

            if (!am) {
                am = node->arrmeta = MemoryContextAllocZero(w->cxt, sizeof(*am));
            }
            ms = &am->meta;
            if (am->valtype != valtype) {
                am->valtype      = valtype;
                ms->element_type = get_element_type(valtype);
                if (OidIsValid(ms->element_type)) {
                    get_typlenbyvalalign(
                        ms->element_type, &ms->typlen, &ms->typbyval, &ms->typalign
                    );
                }
            }
            if (OidIsValid(ms->element_type)) {
                val = pgch_array_from_pg(
                    val, ms->element_type, ms->typlen, ms->typbyval, ms->typalign
                );
                valtype = ANYARRAYOID;
            }
        }
    }

    switch (valtype) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
#if PG_VERSION_NUM >= 190000
    case OID8OID:
#endif
    case XID8OID: {
        int64_t v = 0;

        if (!(kind == CHC_BOOL || (kind >= CHC_INT8 && kind <= CHC_INT64) ||
              (kind >= CHC_UINT8 && kind <= CHC_UINT64))) {
            goto type_mismatch;
        }
        if (!isnull) {
            if (valtype == INT2OID) {
                v = (int64_t)DatumGetInt16(val);
            } else if (valtype == INT4OID) {
                v = (int64_t)DatumGetInt32(val);
            } else {
                /* PostgreSQL xid8 and oid8 use unsigned 64-bit values */
                v = DatumGetInt64(val);
            }
        }
        pgch_append_int(w, col, v, isnull);
        return;
    }
    case BOOLOID:
        if (kind != CHC_BOOL && kind != CHC_UINT8) {
            goto type_mismatch;
        }
        pgch_append_bool(w, col, DatumGetBool(val), isnull);
        return;
    case FLOAT4OID:
        if (kind != CHC_FLOAT32) {
            goto type_mismatch;
        }
        pgch_append_float(w, col, DatumGetFloat4(val), isnull);
        return;
    case FLOAT8OID:
        if (kind != CHC_FLOAT64) {
            goto type_mismatch;
        }
        pgch_append_double(w, col, DatumGetFloat8(val), isnull);
        return;
    case NUMERICOID: {
        char* s = NULL;

        if (kind != CHC_DECIMAL32 && kind != CHC_DECIMAL64 && kind != CHC_DECIMAL128 &&
            kind != CHC_DECIMAL256) {
            goto type_mismatch;
        }
        if (!isnull) {
            Numeric num = DatumGetNumeric(val);

            if (w->nonfinite != PGCH_NONFINITE_KEEP && pgch__numeric_nonfinite(num)) {
                /* Leave digits NULL so nonfinite policy decides NULL or zero */
                isnull = w->nonfinite == PGCH_NONFINITE_NULL;
            } else {
                s = numeric_normalize(num);
            }
            if ((Pointer)num != DatumGetPointer(val)) {
                pfree(num);
            }
        }
        pgch_append_decimal(w, col, s, isnull);
        if (s) {
            pfree(s);
        }
        return;
    }
    case TEXTOID:
    case BYTEAOID: {
        const char* p          = NULL;
        size_t len             = 0;
        struct varlena* string = NULL;

        switch (kind) {
        case CHC_FIXED_STRING:
        case CHC_STRING:
        case CHC_ENUM8:
        case CHC_ENUM16:
            break;
        default:
            goto type_mismatch;
        }
        if (!isnull) {
            /* Packed detoast keeps short varlena headers instead of widening them */
            string = PG_DETOAST_DATUM_PACKED(val);
            p      = VARDATA_ANY(string);
            len    = VARSIZE_ANY_EXHDR(string);
        }
        pgch_append_bytes(w, col, p, len, isnull);
        if (string && (Pointer)string != DatumGetPointer(val)) {
            pfree(string);
        }
        return;
    }
    case DATEOID: {
        int64_t seconds = 0;

        if (kind != CHC_DATE && kind != CHC_DATE32) {
            goto type_mismatch;
        }
        if (!isnull) {
            seconds =
                ((int64_t)DatumGetDateADT(val) + PGCH__DATE_OFFSET) * SECS_PER_DAY;
        }
        pgch_append_date_seconds(w, col, seconds, isnull);
        return;
    }
    case TIMEOID: {
        int64_t usec = isnull ? 0 : (int64_t)DatumGetTimeADT(val);

        switch (kind) {
        case CHC_TIME:
            pgch_append_time_seconds(w, col, usec / USECS_PER_SEC, isnull);
            return;
        case CHC_TIME64: {
            uint32_t scale = pgch_column_datetime64_scale(w, col);
            int64 power    = pgch_pow10[scale];

            pgch_append_time64_raw(
                w,
                col,
                (usec / USECS_PER_SEC) * power +
                    (usec % USECS_PER_SEC) * power / USECS_PER_SEC,
                isnull
            );
            return;
        }
        default:
            goto type_mismatch;
        }
    }
    case TIMESTAMPOID:
    case TIMESTAMPTZOID: {
        /* PostgreSQL timestamp uses session timezone when cast to timestamptz */
        if (valtype == TIMESTAMPOID && !isnull &&
            (kind == CHC_DATETIME || kind == CHC_DATETIME64)) {
            Datum tz;

            if (pgch__cast_value(
                    w, pgch__cursor_node(w, col), val, TIMESTAMPOID, TIMESTAMPTZOID, &tz
                )) {
                val = tz;
            }
        }

        switch (kind) {
        case CHC_DATETIME: {
            int64_t seconds =
                isnull ? 0 : (int64_t)timestamptz_to_time_t(DatumGetTimestamp(val));

            pgch_append_datetime_seconds(w, col, seconds, isnull);
        } break;
        case CHC_DATETIME64: {
            int64_t raw = 0;

            if (!isnull) {
                uint32_t scale = pgch_column_datetime64_scale(w, col);
                Timestamp t    = DatumGetTimestamp(val);
                int64 power    = pgch_pow10[scale];
                int64 secs     = t / USECS_PER_SEC;
                int64 us_rem   = t % USECS_PER_SEC;

                if (us_rem < 0) {
                    secs -= 1;
                    us_rem += USECS_PER_SEC;
                }
                secs += PGCH__DATE_OFFSET * SECS_PER_DAY;
                if (pg_mul_s64_overflow(secs, power, &raw) ||
                    pg_add_s64_overflow(raw, us_rem * power / USECS_PER_SEC, &raw)) {
                    pgch_errorf(
                        ERRCODE_DATETIME_VALUE_OUT_OF_RANGE,
                        "timestamp out of range for %s",
                        chc_type_name(pgch__cursor_node(w, col)->type, NULL)
                    );
                }
            }
            pgch_append_datetime64_raw(w, col, raw, isnull);
        } break;
        default:
            goto type_mismatch;
        }
        return;
    }
    case ANYARRAYOID: {
        pgch_array* arr;
        chc_kind item_kind;
        Oid child_valtype;

        if (kind != CHC_ARRAY) {
            goto type_mismatch;
        }
        if (isnull) {
            pgch__append_null_array(w, col);
            return;
        }

        arr = (pgch_array*)DatumGetPointer(val);
        pgch_array_begin(w, col);

        item_kind     = pgch_column_kind(w, col);
        child_valtype = (arr->ndim > 1) ? ANYARRAYOID : arr->item_type;
        for (size_t i = 0; i < arr->len; i++) {
            pgch__append_one(
                w, 0, item_kind, arr->datums[i], child_valtype, arr->nulls[i]
            );
        }

        pgch_array_end(w);
        return;
    }
    case UUIDOID: {
        uint8_t bytes[16];

        if (kind != CHC_UUID) {
            goto type_mismatch;
        }
        if (!isnull) {
            memcpy(bytes, DatumGetUUIDP(val)->data, 16);
        } else {
            memset(bytes, 0, 16);
        }
        pgch_append_uuid(w, col, bytes, isnull);
        return;
    }
    case INETOID: {
        const uint8_t* addr = NULL;
        size_t addrlen      = 0;

        if (kind != CHC_IPV4 && kind != CHC_IPV6) {
            goto type_mismatch;
        }
        if (!isnull) {
            inet* ipa    = DatumGetInetPP(val);
            int fam      = ip_family(ipa);
            int expected = (kind == CHC_IPV4) ? PGSQL_AF_INET : PGSQL_AF_INET6;

            if (fam != expected) {
                pgch_errorf(
                    ERRCODE_DATATYPE_MISMATCH,
                    "inet family mismatch for %s",
                    pgch__col_desc(w, col)
                );
            }
            addr    = ip_addr(ipa);
            addrlen = ip_addrsize(ipa);
        } else {
            addrlen = (kind == CHC_IPV4) ? 4 : 16;
        }
        pgch_append_inet(w, col, addr, addrlen, isnull);
        return;
    }
    case JSONOID:
    case JSONBOID: {
        const char* p          = NULL;
        size_t len             = 0;
        struct varlena* string = NULL;
        StringInfoData buf     = {};

        if (kind != CHC_JSON && kind != CHC_OBJECT && kind != CHC_STRING) {
            goto type_mismatch;
        }
        if (!isnull) {
            if (valtype == JSONOID) {
                /* PostgreSQL stores json as text */
                string = PG_DETOAST_DATUM_PACKED(val);
                p      = VARDATA_ANY(string);
                len    = VARSIZE_ANY_EXHDR(string);
            } else {
                Jsonb* jb = DatumGetJsonbP(val);

                initStringInfo(&buf);
                JsonbToCString(&buf, &jb->root, VARSIZE(jb));
                p   = buf.data;
                len = buf.len;
                if ((Pointer)jb != DatumGetPointer(val)) {
                    pfree(jb);
                }
            }
        }
        pgch_append_bytes(w, col, p, len, isnull);
        if (string && (Pointer)string != DatumGetPointer(val)) {
            pfree(string);
        }
        if (buf.data) {
            pfree(buf.data);
        }
        return;
    }
    default:
        goto type_mismatch;
    }

type_mismatch: {
    const chc_type* ct = w->cols[pgch__target_col(w, col)].t;
    pgch__node* node   = pgch__cursor_node(w, col);
    Datum conv;

    if (!OidIsValid(node->target)) {
        node->target = pgch_native_oid_for(node->type, pgch__col_desc(w, col));
    }
    if (node->target != valtype) {
        if (isnull) {
            pgch__append_one(w, col, kind, val, node->target, isnull);
            return;
        }
        if (pgch__cast_value(w, node, val, valtype, node->target, &conv)) {
            pgch__append_one(w, col, kind, conv, node->target, isnull);
            return;
        }
    }

    pgch_errorf(
        ERRCODE_DATATYPE_MISMATCH,
        "cannot encode %s into ClickHouse %s (%s)",
        format_type_be(valtype),
        chc_type_name(ct, NULL),
        pgch__col_desc(w, col)
    );
}
}

void
pgch_append_datum(pgch_writer* w, size_t col, Datum val, Oid valtype, bool isnull) {
    if (!w->cursor_len && col >= w->ncols) {
        pgch_errorf(ERRCODE_FDW_ERROR, "column %zu out of range", col);
    }
    pgch__append_one(w, col, pgch_column_kind(w, col), val, valtype, isnull);
}

void
pgch_append_slot(pgch_writer* w, TupleTableSlot* slot) {
    TupleDesc desc = slot->tts_tupleDescriptor;
    size_t col     = 0;

    slot_getallattrs(slot);
    for (int i = 0; i < desc->natts; i++) {
        Form_pg_attribute a = TupleDescAttr(desc, i);

        if (!pgch_attr_is_streamed(a)) {
            continue;
        }
        pgch_append_datum(
            w, col++, slot->tts_values[i], a->atttypid, slot->tts_isnull[i]
        );
    }
}

/* ---- block assembly ------------------------------------------------- */

typedef struct pgch_lcd_key {
    const uint8_t* bytes;
    size_t len;
} pgch_lcd_key;

typedef struct pgch_lcd_entry {
    uint32 status;
    pgch_lcd_key key;
    uint32 idx;
} pgch_lcd_entry;

#define SH_PREFIX pgch_lcd
#define SH_ELEMENT_TYPE pgch_lcd_entry
#define SH_KEY_TYPE pgch_lcd_key
#define SH_KEY key
#define SH_HASH_KEY(tb, key) hash_bytes((key).bytes, (int)(key).len)
#define SH_EQUAL(tb, a, b)                                                             \
    ((a).len == (b).len && memcmp((a).bytes, (b).bytes, (a).len) == 0)
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

static void
pgch__build_lc_dict(
    const pgch__node* node,
    uint64_t** out_dict_offs,
    uint8_t** out_dict_data,
    size_t* out_dict_n,
    void** out_keys,
    int* out_key_size,
    size_t* out_n_rows
) {
    bool nullable            = node->lc.inner_nullable;
    const uint64_t* row_offs = pgch__offs_data(&node->lc.offs);
    size_t n_rows            = pgch__offs_len(&node->lc.offs);
    uint64_t* dict_offs      = NULL;
    uint8_t* dict_data       = NULL;
    uint32_t* keys           = n_rows ? palloc(n_rows * sizeof(uint32_t)) : NULL;
    size_t dict_n            = 0;
    size_t dict_cap          = 0;
    size_t data_len          = 0;
    pgch_lcd_hash* ht =
        n_rows
            ? pgch_lcd_create(
                  CurrentMemoryContext, (uint32)Min(n_rows, (size_t)PG_UINT32_MAX), NULL
              )
            : NULL;

    if (nullable) {
        /* ClickHouse reserves dictionary entry zero for NULL */
        dict_cap     = 8;
        dict_offs    = palloc(dict_cap * sizeof(uint64_t));
        dict_offs[0] = 0;
        dict_n       = 1;
    }

    for (size_t i = 0; i < n_rows; i++) {
        uint64_t start       = i == 0 ? 0 : row_offs[i - 1];
        uint64_t end         = row_offs[i];
        size_t len           = (size_t)(end - start);
        const uint8_t* bytes = node->lc.data.data + start;
        pgch_lcd_key k       = { bytes, len };
        pgch_lcd_entry* entry;
        bool found;

        if (nullable && node->lc.null_map.data[i]) {
            keys[i] = 0;
            continue;
        }

        entry = pgch_lcd_insert(ht, k, &found);
        if (found) {
            keys[i] = entry->idx;
            continue;
        }

        if (dict_n == dict_cap) {
            dict_cap  = dict_cap ? dict_cap * 2 : 64;
            dict_offs = dict_offs ? repalloc(dict_offs, dict_cap * sizeof(uint64_t))
                                  : palloc(dict_cap * sizeof(uint64_t));
        }
        data_len += len;
        dict_offs[dict_n] = data_len;
        entry->idx        = (uint32)dict_n;
        keys[i]           = (uint32)dict_n;
        dict_n++;
    }

    if (data_len) {
        pgch_lcd_iterator it;
        pgch_lcd_entry* e;

        dict_data =
            MemoryContextAllocExtended(CurrentMemoryContext, data_len, MCXT_ALLOC_HUGE);
        pgch_lcd_start_iterate(ht, &it);
        while ((e = pgch_lcd_iterate(ht, &it)) != NULL) {
            uint64_t s = e->idx == 0 ? 0 : dict_offs[e->idx - 1];

            memcpy(dict_data + s, e->key.bytes, e->key.len);
        }
    }
    if (ht) {
        pgch_lcd_destroy(ht);
    }
    *out_dict_offs = dict_offs;
    *out_dict_data = dict_data;
    *out_dict_n    = dict_n;
    *out_keys      = keys;
    *out_key_size  = 4;
    *out_n_rows    = n_rows;
}

static inline chc_column*
pgch__col_node(chc_column v) {
    chc_column* n = palloc(sizeof(*n));

    *n = v;
    return n;
}

static chc_column*
pgch__finalize_node(pgch__node* n) {
    switch (n->kind) {
    case PGCH__FIXED:
        return pgch__col_node(
            chc_build_fixed(n->fixed.data.data, n->fixed.elem_size, pgch__node_rows(n))
        );
    case PGCH__STRING:
        return pgch__col_node(chc_build_string(
            pgch__offs_data(&n->str.offs),
            n->str.data.data,
            pgch__offs_len(&n->str.offs)
        ));
    case PGCH__NULLABLE:
        return pgch__col_node(chc_build_nullable(
            n->nullable.null_map.data, pgch__finalize_node(n->nullable.inner)
        ));
    case PGCH__ARRAY:
        return pgch__col_node(chc_build_array(
            pgch__offs_data(&n->array.offs),
            pgch__offs_len(&n->array.offs),
            pgch__finalize_node(n->array.values)
        ));
    case PGCH__LC: {
        size_t dict_n, n_rows;
        int key_size;
        uint64_t* lc_offs;
        uint8_t* lc_data;
        void* lc_keys;

        pgch__build_lc_dict(
            n, &lc_offs, &lc_data, &dict_n, &lc_keys, &key_size, &n_rows
        );
        chc_column* dict = pgch__col_node(chc_build_string(lc_offs, lc_data, dict_n));

        return pgch__col_node(chc_build_lc(key_size, lc_keys, n_rows, dict));
    }
    }
    pg_unreachable();
}

static size_t
pgch__node_bytes(const pgch__node* n) {
    switch (n->kind) {
    case PGCH__FIXED:
        return n->fixed.data.len;
    case PGCH__STRING:
        return n->str.data.len + n->str.offs.len;
    case PGCH__NULLABLE:
        return n->nullable.null_map.len + pgch__node_bytes(n->nullable.inner);
    case PGCH__ARRAY:
        return n->array.offs.len + pgch__node_bytes(n->array.values);
    case PGCH__LC:
        return n->lc.data.len + n->lc.offs.len + n->lc.null_map.len;
    }
    pg_unreachable();
}

static void
pgch__reset_node(pgch__node* n) {
    switch (n->kind) {
    case PGCH__FIXED:
        pgch_buf_reset(&n->fixed.data);
        return;
    case PGCH__STRING:
        pgch_buf_reset(&n->str.data);
        pgch_buf_reset(&n->str.offs);
        return;
    case PGCH__NULLABLE:
        pgch_buf_reset(&n->nullable.null_map);
        pgch__reset_node(n->nullable.inner);
        return;
    case PGCH__ARRAY:
        pgch_buf_reset(&n->array.offs);
        pgch__reset_node(n->array.values);
        return;
    case PGCH__LC:
        pgch_buf_reset(&n->lc.data);
        pgch_buf_reset(&n->lc.offs);
        pgch_buf_reset(&n->lc.null_map);
        return;
    }
    pg_unreachable();
}

size_t
pgch_writer_rows(const pgch_writer* w) {
    return w->ncols ? pgch__node_rows(w->cols[0].root) : 0;
}

size_t
pgch_writer_bytes(const pgch_writer* w) {
    size_t total = 0;

    for (size_t i = 0; i < w->ncols; i++) {
        total += pgch__node_bytes(w->cols[i].root);
    }
    return total;
}

const chc_block_builder*
pgch_writer_build(pgch_writer* w) {
    MemoryContext old;
    chc_block_col* bcols;

    /* Close every array context before building block */
    Assert(w->cursor_len == 0);

    if (w->bcxt) {
        MemoryContextDelete(w->bcxt);
    }
    w->bcxt = AllocSetContextCreate(w->cxt, "pgch block", ALLOCSET_DEFAULT_SIZES);
    old     = MemoryContextSwitchTo(w->bcxt);

    bcols = w->ncols ? palloc(w->ncols * sizeof(*bcols)) : NULL;
    chc_block_builder_init(&w->bb, bcols);

    for (size_t i = 0; i < w->ncols; i++) {
        pgch__col* c = &w->cols[i];

        chc_block_builder_append(
            &w->bb, c->name, c->name_len, c->t, pgch__finalize_node(c->root)
        );
    }

    MemoryContextSwitchTo(old);
    return &w->bb;
}

void
pgch_writer_reset(pgch_writer* w) {
    if (w->bcxt) {
        MemoryContextDelete(w->bcxt);
        w->bcxt = NULL;
    }
    chc_block_builder_init(&w->bb, NULL);
    w->cursor_len = 0;
    for (size_t i = 0; i < w->ncols; i++) {
        pgch__reset_node(w->cols[i].root);
    }
}

void
pgch_writer_flush(pgch_writer* w, pgch_buf* out, const chc_block_opts* opts) {
    const chc_block_builder* bb = pgch_writer_build(w);
    chc_io io;
    chc_err err = {};

    pgch_buf_io(out, &io);
    if (!opts) {
        opts = &pgch_block_opts_local;
    }
    if (chc_block_write(&io, bb, opts, &err) != CHC_OK) {
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ");
    }
    pgch_writer_reset(w);
}

#endif /* PGCH_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* PG_CLICKHOUSE_ENCODE_H */
