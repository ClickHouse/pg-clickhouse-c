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

/* Caller owns checkpoint, initialize to zero before first use */
typedef struct pgch_checkpoint {
    void* entries;
    size_t nentries;
    size_t capacity;
    const pgch_writer* writer;
    uint64 generation;
} pgch_checkpoint;

/*
 * Create writer in a child context of parent
 * Copy column names, borrow column types, reject unsupported types
 */
extern pgch_writer*
pgch_writer_new(MemoryContext parent, const pgch_col* cols, size_t ncols);

extern void
pgch_writer_free(pgch_writer* w);

/* Choose whether NULL arrays raise or become empty arrays */
typedef enum pgch_null_array {
    PGCH_NULL_ARRAY_ERROR = 0,
    PGCH_NULL_ARRAY_EMPTY,
} pgch_null_array;

extern void
pgch_writer_set_null_array(pgch_writer* w, pgch_null_array policy);

/*
 * Save or restore row write position, no array or tuple may be open when saving
 * Allocate storage in CurrentMemoryContext, reuse it on later saves
 */
extern void
pgch_writer_checkpoint(pgch_writer* w, pgch_checkpoint* checkpoint);

extern void
pgch_writer_rollback(pgch_writer* w, const pgch_checkpoint* checkpoint);

extern void
pgch_checkpoint_free(pgch_checkpoint* checkpoint);

/*
 * Append PostgreSQL value to column
 * Pass value OID in valtype, or ANYARRAYOID for pgch_array
 * Append exactly one value to every column in each row
 * Map uses an array of key-value pairs, Tuple uses an array of fields
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

/*
 * Open array element context, appends targeting the element column
 * Nest calls for nested arrays, close each call with pgch_array_end
 */
extern void
pgch_array_begin(pgch_writer* w, size_t col);
extern void
pgch_array_end(pgch_writer* w);

/*
 * Open tuple field context, appends filling fields left to right
 * Close with pgch_tuple_end, which requires every field filled
 * Write Map(K, V) as Array(Tuple(K, V)), which is how ClickHouse stores it
 */
extern void
pgch_tuple_begin(pgch_writer* w, size_t col);
extern void
pgch_tuple_end(pgch_writer* w);

/* Return true while any array or tuple context is open */
extern bool
pgch_nest_active(const pgch_writer* w);

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
#include "utils/geo_decls.h"
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

typedef struct pgch__node pgch__node;

typedef struct pgch__node_checkpoint {
    size_t data;
    size_t offsets;
    size_t nulls;
} pgch__node_checkpoint;

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

/* Buffer per clickhouse-c column node, so a tree finalizes into chc_column */
struct pgch__node {
    chc_col_kind layout;
    chc_kind kind; /* Geo levels stand in for types the caller never named */
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
            pgch__node** children;
            size_t arity;
        } tuple;

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

/* Open Array or Tuple node, child indexing the Tuple field taking next value */
typedef struct pgch__frame {
    pgch__node* node;
    size_t child;
} pgch__frame;

struct pgch_writer {
    MemoryContext cxt;
    MemoryContext bcxt;

    size_t ncols;
    pgch__col* cols;

    pgch_null_array null_array;

    chc_block_builder bb;

    pgch__frame* cursor;
    size_t cursor_len;
    size_t cursor_cap;
    size_t cursor_col;

    uint64 generation;
};

static void
pgch__geo_fill(pgch__node* n, chc_kind kind);

static pgch__node*
pgch__geo_node(chc_kind kind) {
    pgch__node* n = palloc0(sizeof(pgch__node));

    pgch__geo_fill(n, kind);
    return n;
}

/*
 * Geo types nest Arrays over Point, which is Tuple(Float64, Float64): Ring and
 * LineString one level, Polygon and MultiLineString two, MultiPolygon three
 */
static void
pgch__geo_fill(pgch__node* n, chc_kind kind) {
    chc_kind inner;

    n->kind = kind;
    if (kind == CHC_POINT) {
        n->layout         = CHC_COL_TUPLE;
        n->tuple.arity    = 2;
        n->tuple.children = palloc0(n->tuple.arity * sizeof(pgch__node*));
        for (size_t i = 0; i < n->tuple.arity; i++) {
            pgch__node* axis = palloc0(sizeof(pgch__node));

            axis->layout          = CHC_COL_FIXED;
            axis->kind            = CHC_FLOAT64;
            axis->fixed.elem_size = 8;
            n->tuple.children[i]  = axis;
        }
        return;
    }

    switch (kind) {
    case CHC_RING:
    case CHC_LINE_STRING:
        inner = CHC_POINT;
        break;
    case CHC_POLYGON:
        inner = CHC_RING;
        break;
    case CHC_MULTI_POLYGON:
        inner = CHC_POLYGON;
        break;
    case CHC_MULTI_LINE_STRING:
        inner = CHC_LINE_STRING;
        break;
    default:
        pg_unreachable();
    }
    n->layout       = CHC_COL_ARRAY;
    n->array.values = pgch__geo_node(inner);
}

static pgch__node*
pgch__node_new(const chc_type* t);

/* Fill Tuple node over first arity children, the pair being a Map's key & value */
static void
pgch__tuple_fill(pgch__node* n, const chc_type* t, size_t arity) {
    n->layout         = CHC_COL_TUPLE;
    n->tuple.arity    = arity;
    n->tuple.children = palloc0(arity * sizeof(pgch__node*));
    for (size_t i = 0; i < arity; i++) {
        n->tuple.children[i] = pgch__node_new(chc_type_child(t, i));
    }
}

static pgch__node*
pgch__node_new(const chc_type* t) {
    pgch__node* n = palloc0(sizeof(pgch__node));

    n->type = t;
    n->kind = chc_type_kind(t);
    switch (chc_type_kind(t)) {
    case CHC_NULLABLE:
        n->layout         = CHC_COL_NULLABLE;
        n->nullable.inner = pgch__node_new(chc_type_child(t, 0));
        return n;
    case CHC_ARRAY:
        n->layout       = CHC_COL_ARRAY;
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
        n->layout            = CHC_COL_LOW_CARDINALITY;
        n->lc.inner_nullable = inner_nullable;
        return n;
    }
    case CHC_STRING:
        n->layout = CHC_COL_STRING;
        return n;
    case CHC_JSON:
        n->layout      = CHC_COL_STRING;
        n->str.is_json = true;
        return n;
    case CHC_POINT:
    case CHC_RING:
    case CHC_LINE_STRING:
    case CHC_POLYGON:
    case CHC_MULTI_POLYGON:
    case CHC_MULTI_LINE_STRING:
        pgch__geo_fill(n, chc_type_kind(t));
        return n;
    case CHC_TUPLE: {
        size_t arity = chc_type_n_children(t);

        if (arity == 0) {
            pgch_error(ERRCODE_FDW_INVALID_DATA_TYPE, "Tuple column has no fields");
        }
        pgch__tuple_fill(n, t, arity);
        return n;
    }
    case CHC_MAP: {
        /* Map is Array(Tuple(K, V)), which clickhouse-c writes without a node */
        pgch__node* entries = palloc0(sizeof(pgch__node));

        if (chc_type_n_children(t) != 2) {
            pgch_error(ERRCODE_FDW_INVALID_DATA_TYPE, "Map wants key and value");
        }
        entries->kind = CHC_TUPLE;
        pgch__tuple_fill(entries, t, 2);
        n->layout       = CHC_COL_ARRAY;
        n->array.values = entries;
        return n;
    }
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
        n->layout           = CHC_COL_FIXED;
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
        n->layout          = CHC_COL_FIXED;
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

static size_t
pgch__node_count(const pgch__node* n) {
    size_t count = 1;

    switch (n->layout) {
    case CHC_COL_NULLABLE:
        return count + pgch__node_count(n->nullable.inner);
    case CHC_COL_ARRAY:
        return count + pgch__node_count(n->array.values);
    case CHC_COL_TUPLE:
        for (size_t i = 0; i < n->tuple.arity; i++) {
            count += pgch__node_count(n->tuple.children[i]);
        }
        return count;
    case CHC_COL_FIXED:
    case CHC_COL_STRING:
    case CHC_COL_LOW_CARDINALITY:
        return count;
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

static void
pgch__checkpoint_node(
    const pgch__node* n,
    pgch__node_checkpoint* entries,
    size_t* pos
) {
    pgch__node_checkpoint* checkpoint = &entries[(*pos)++];

    *checkpoint = (pgch__node_checkpoint){};
    switch (n->layout) {
    case CHC_COL_FIXED:
        checkpoint->data = n->fixed.data.len;
        return;
    case CHC_COL_STRING:
        checkpoint->data    = n->str.data.len;
        checkpoint->offsets = n->str.offs.len;
        return;
    case CHC_COL_NULLABLE:
        checkpoint->nulls = n->nullable.null_map.len;
        pgch__checkpoint_node(n->nullable.inner, entries, pos);
        return;
    case CHC_COL_ARRAY:
        checkpoint->offsets = n->array.offs.len;
        pgch__checkpoint_node(n->array.values, entries, pos);
        return;
    case CHC_COL_TUPLE:
        for (size_t i = 0; i < n->tuple.arity; i++) {
            pgch__checkpoint_node(n->tuple.children[i], entries, pos);
        }
        return;
    case CHC_COL_LOW_CARDINALITY:
        checkpoint->data    = n->lc.data.len;
        checkpoint->offsets = n->lc.offs.len;
        checkpoint->nulls   = n->lc.null_map.len;
        return;
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

static void
pgch__rollback_node(pgch__node* n, const pgch__node_checkpoint* entries, size_t* pos) {
    const pgch__node_checkpoint* checkpoint = &entries[(*pos)++];

    switch (n->layout) {
    case CHC_COL_FIXED:
        n->fixed.data.len = checkpoint->data;
        return;
    case CHC_COL_STRING:
        n->str.data.len = checkpoint->data;
        n->str.offs.len = checkpoint->offsets;
        return;
    case CHC_COL_NULLABLE:
        n->nullable.null_map.len = checkpoint->nulls;
        pgch__rollback_node(n->nullable.inner, entries, pos);
        return;
    case CHC_COL_ARRAY:
        n->array.offs.len = checkpoint->offsets;
        pgch__rollback_node(n->array.values, entries, pos);
        return;
    case CHC_COL_TUPLE:
        for (size_t i = 0; i < n->tuple.arity; i++) {
            pgch__rollback_node(n->tuple.children[i], entries, pos);
        }
        return;
    case CHC_COL_LOW_CARDINALITY:
        n->lc.data.len     = checkpoint->data;
        n->lc.offs.len     = checkpoint->offsets;
        n->lc.null_map.len = checkpoint->nulls;
        return;
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

void
pgch_writer_checkpoint(pgch_writer* w, pgch_checkpoint* checkpoint) {
    size_t nentries = 0;
    size_t pos      = 0;

    if (w->cursor_len) {
        pgch_error(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE, "checkpoint inside nested value"
        );
    }
    for (size_t i = 0; i < w->ncols; i++) {
        nentries += pgch__node_count(w->cols[i].root);
    }
    if (checkpoint->capacity < nentries) {
        checkpoint->entries =
            checkpoint->entries
                ? repalloc(
                      checkpoint->entries, nentries * sizeof(pgch__node_checkpoint)
                  )
                : palloc(nentries * sizeof(pgch__node_checkpoint));
        checkpoint->capacity = nentries;
    }
    for (size_t i = 0; i < w->ncols; i++) {
        pgch__checkpoint_node(w->cols[i].root, checkpoint->entries, &pos);
    }
    checkpoint->nentries   = nentries;
    checkpoint->writer     = w;
    checkpoint->generation = w->generation;
}

void
pgch_writer_rollback(pgch_writer* w, const pgch_checkpoint* checkpoint) {
    size_t pos = 0;

    if (checkpoint->writer != w || checkpoint->generation != w->generation) {
        pgch_error(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE, "stale writer checkpoint");
    }
    if (w->bcxt) {
        MemoryContextDelete(w->bcxt);
        w->bcxt = NULL;
    }
    chc_block_builder_init(&w->bb, NULL);
    w->cursor_len = 0;
    for (size_t i = 0; i < w->ncols; i++) {
        pgch__rollback_node(w->cols[i].root, checkpoint->entries, &pos);
    }
    if (pos != checkpoint->nentries) {
        pgch_error(ERRCODE_INTERNAL_ERROR, "writer checkpoint shape changed");
    }
    w->generation++;
}

void
pgch_checkpoint_free(pgch_checkpoint* checkpoint) {
    if (checkpoint->entries) {
        pfree(checkpoint->entries);
    }
    *checkpoint = (pgch_checkpoint){};
}

/* Return node taking next value: an array's element or a tuple's current field */
static inline pgch__node*
pgch__cursor_node(const pgch_writer* w, size_t col) {
    if (!w->cursor_len) {
        return w->cols[col].root;
    }
    const pgch__frame* f = &w->cursor[w->cursor_len - 1];
    if (f->node->layout == CHC_COL_ARRAY) {
        return f->node->array.values;
    }
    if (f->child >= f->node->tuple.arity) {
        pgch_errorf(ERRCODE_FDW_ERROR, "Tuple takes %zu values", f->node->tuple.arity);
    }
    return f->node->tuple.children[f->child];
}

/* Move past the field just filled, array elements repeating one node instead */
static inline void
pgch__cursor_step(pgch_writer* w) {
    if (w->cursor_len) {
        pgch__frame* f = &w->cursor[w->cursor_len - 1];

        if (f->node->layout == CHC_COL_TUPLE) {
            f->child++;
        }
    }
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

pg_noreturn static void
pgch__null_violation(pgch_writer* w, size_t col) {
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

static pgch__node*
pgch__resolve_leaf(pgch_writer* w, size_t col, bool isnull) {
    uint8_t b     = isnull ? 1 : 0;
    bool nullable = false;

    if (!w->cursor_len && col >= w->ncols) {
        pgch_errorf(ERRCODE_FDW_ERROR, "column %zu out of range", col);
    }
    pgch__node* node = pgch__cursor_node(w, col);
    pgch__cursor_step(w);

    while (node->layout == CHC_COL_NULLABLE) {
        pgch_buf_append(&node->nullable.null_map, &b, 1);
        nullable = true;
        node     = node->nullable.inner;
    }
    if (node->layout == CHC_COL_LOW_CARDINALITY && node->lc.inner_nullable) {
        pgch_buf_append(&node->lc.null_map, &b, 1);
        nullable = true;
    }
    if (isnull && !nullable) {
        pgch__null_violation(w, col);
    }
    if (node->layout == CHC_COL_ARRAY) {
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
    if (node->layout != CHC_COL_FIXED) {
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

/* Decimal and ClickHouse integers outside bigint's range map from numeric */
static inline bool
pgch__kind_maps_to_numeric(chc_kind kind) {
    return (kind >= CHC_DECIMAL32 && kind <= CHC_DECIMAL256) || kind == CHC_UINT64 ||
           kind == CHC_INT128 || kind == CHC_UINT128 || kind == CHC_INT256 ||
           kind == CHC_UINT256;
}

/* ClickHouse stores Decimal and wide integers as little-endian two's complement */
static void
pgch__number_to_bytes(const char* s, const chc_type* type, size_t width, uint8_t* out) {
    const char* input = s;
    const char* name  = chc_type_name(type, NULL);
    uint32_t scale    = (uint32_t)chc_type_decimal_scale(type);
    bool is_signed    = !pgch_kind_is_unsigned(chc_type_kind(type));
    bool neg          = false;

    if (!s) {
        pgch_error(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, "numeric parse failure");
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
                "cannot encode \"%s\" as ClickHouse %s",
                input,
                name
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
    /* Signed widths spend their top bit on the sign, bar the negative extreme */
    if (is_signed && (mag[nwords - 1] & 0x80000000u)) {
        spilled |= !neg || mag[nwords - 1] != 0x80000000u;
        for (size_t b = 0; !spilled && b + 1 < nwords; b++) {
            spilled = mag[b] != 0;
        }
    }
    if (spilled) {
        pgch_errorf(
            ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
            "value \"%s\" out of range for ClickHouse %s",
            input,
            name
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

    for (size_t b = 0; b < nwords; b++) {
        uint32_t word = PGCH__LE32(mag[b]);

        memcpy(out + b * sizeof word, &word, sizeof word);
    }
}

/* ---- little-endian fixed-width writes, one per width ---------------- */

/*
 * Generate fixed-width scalar writers. For example, i16 invocation below
 * defines pgch__append_i16(), which narrows int64_t argument to int16_t, writes
 * zero for NULL, converts bits to little-endian order, and appends two bytes
 */
#define PGCH__APPEND_SCALAR(suffix, T, ARG, U, LE)                                     \
    static void pgch__append_##suffix(                                                 \
        pgch_writer* w, size_t col, ARG val, bool isnull                               \
    ) {                                                                                \
        MemoryContext old = MemoryContextSwitchTo(w->cxt);                             \
        pgch__node* node  = pgch__resolve_leaf(w, col, isnull);                        \
        T v               = isnull ? 0 : (T)val;                                       \
        U u;                                                                           \
                                                                                       \
        memcpy(&u, &v, sizeof u);                                                      \
        u = LE(u);                                                                     \
        pgch_buf_append(pgch__fixed_data(node), &u, sizeof u);                         \
        MemoryContextSwitchTo(old);                                                    \
    }

PGCH__APPEND_SCALAR(i8, int8_t, int64_t, uint8_t, PGCH__LE8)
PGCH__APPEND_SCALAR(i16, int16_t, int64_t, uint16_t, PGCH__LE16)
PGCH__APPEND_SCALAR(i32, int32_t, int64_t, uint32_t, PGCH__LE32)
PGCH__APPEND_SCALAR(i64, int64_t, int64_t, uint64_t, PGCH__LE64)
PGCH__APPEND_SCALAR(f32, float, double, uint32_t, PGCH__LE32)
PGCH__APPEND_SCALAR(f64, double, double, uint64_t, PGCH__LE64)

/* ClickHouse truncates a Float32 to its leading 16 bits, so match, do not round */
static void
pgch__append_bf16(pgch_writer* w, size_t col, float val, bool isnull) {
    uint32_t bits;

    memcpy(&bits, &val, sizeof bits);
    pgch__append_i16(w, col, (int16_t)(uint16_t)(bits >> 16), isnull);
}

/* PostgreSQL interval to ClickHouse ticks. Months kept separate from days and time */
static int64
pgch__interval_raw(const Interval* iv, Interval unit, const char* name) {
    int64 usec;

    if (unit.month) {
        if (!iv->day && !iv->time && iv->month % unit.month == 0) {
            return iv->month / unit.month;
        }
    } else if (
        !iv->month && !pg_mul_s64_overflow((int64)iv->day, USECS_PER_DAY, &usec) &&
        !pg_add_s64_overflow(usec, iv->time, &usec)
    ) {
        int64 per = unit.day * USECS_PER_DAY + unit.time;

        if (per) {
            if (usec % per == 0) {
                return usec / per;
            }
        } else {
            int64 nsec;

            if (!pg_mul_s64_overflow(usec, 1000, &nsec)) {
                return nsec;
            }
        }
    }
    pgch_errorf(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "interval does not fit %s", name);
}

static void
pgch__append_interval(pgch_writer* w, size_t col, const Interval* iv, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    uint64_t raw      = 0;

    if (!isnull) {
        raw = (uint64_t)pgch__interval_raw(
            iv, pgch_interval_unit_of(node->type), chc_type_name(node->type, NULL)
        );
    }
    raw = PGCH__LE64(raw);
    pgch_buf_append(pgch__fixed_data(node), &raw, sizeof raw);
    MemoryContextSwitchTo(old);
}

/* Fixed-width leaf takes n bytes as given, NULL rows taking zeros */
static void
pgch__append_raw(pgch_writer* w, size_t col, const void* p, size_t n, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);

    if (isnull) {
        pgch_buf_append_zero(pgch__fixed_data(node), n);
    } else {
        pgch_buf_append(pgch__fixed_data(node), p, n);
    }
    MemoryContextSwitchTo(old);
}

static void
pgch__append_bytes_fixed(pgch__node* node, const void* p, size_t n, bool isnull) {
    chc_kind k     = node->kind;
    pgch_buf* data = &node->fixed.data;

    if (k == CHC_FIXED_STRING) {
        size_t width = node->fixed.elem_size;

        if (isnull) {
            pgch_buf_append_zero(data, width);
            return;
        }
        /* ClickHouse rejects over-long values instead of truncating them */
        if (n > width) {
            pgch_errorf(
                ERRCODE_STRING_DATA_RIGHT_TRUNCATION,
                "value of %zu bytes too long for FixedString(%zu)",
                n,
                width
            );
        }
        if (n) {
            pgch_buf_append(data, p, n);
        }
        pgch_buf_append_zero(data, width - n);
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
            uint16_t v = PGCH__LE16((uint16_t)(int16_t)val);

            pgch_buf_append(data, &v, 2);
        }
        return;
    }
    pgch_error(ERRCODE_DATATYPE_MISMATCH, "bytes into non-text column");
}

static void
pgch__append_bytes(pgch_writer* w, size_t col, const void* p, size_t n, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);

    switch (node->layout) {
    case CHC_COL_LOW_CARDINALITY:
        pgch__append_row_offs(
            &node->lc.data, &node->lc.offs, isnull ? NULL : p, isnull ? 0 : n
        );
        break;
    case CHC_COL_STRING:
        if (isnull && node->str.is_json) {
            /* ClickHouse validates JSON values even when null map marks NULL */
            pgch__append_row_offs(&node->str.data, &node->str.offs, "{}", 2);
        } else {
            pgch__append_row_offs(
                &node->str.data, &node->str.offs, isnull ? NULL : p, isnull ? 0 : n
            );
        }
        break;
    case CHC_COL_FIXED:
        pgch__append_bytes_fixed(node, p, n, isnull);
        break;
    default:
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "bytes into non-text column");
    }
    MemoryContextSwitchTo(old);
}

static void
pgch__append_number(pgch_writer* w, size_t col, const char* digits, bool isnull) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);
    pgch_buf* data    = pgch__fixed_data(node);
    size_t width      = node->fixed.elem_size;
    uint8_t raw[32]   = {};

    if (!pgch__kind_maps_to_numeric(node->kind)) {
        pgch_error(ERRCODE_DATATYPE_MISMATCH, "numeric into non-numeric column");
    }
    if (!isnull) {
        pgch__number_to_bytes(digits, node->type, width, raw);
    }
    pgch_buf_append(data, raw, width);
    MemoryContextSwitchTo(old);
}

static uint64_t
pgch__node_rows(const pgch__node* n) {
    switch (n->layout) {
    case CHC_COL_FIXED:
        return n->fixed.elem_size ? n->fixed.data.len / n->fixed.elem_size : 0;
    case CHC_COL_STRING:
        return pgch__offs_len(&n->str.offs);
    case CHC_COL_NULLABLE:
        return pgch__node_rows(n->nullable.inner);
    case CHC_COL_ARRAY:
        return pgch__offs_len(&n->array.offs);
    case CHC_COL_TUPLE:
        return pgch__node_rows(n->tuple.children[0]);
    case CHC_COL_LOW_CARDINALITY:
        return pgch__offs_len(&n->lc.offs);
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

static void
pgch__cursor_push(pgch_writer* w, size_t col, chc_col_kind layout) {
    const char* what = layout == CHC_COL_ARRAY ? "Array" : "Tuple";
    pgch__node* node;

    if (w->cursor_len) {
        node = pgch__cursor_node(w, col);
    } else {
        if (col >= w->ncols) {
            pgch_errorf(ERRCODE_FDW_ERROR, "column %zu out of range", col);
        }
        node = w->cols[col].root;
    }

    MemoryContext old = MemoryContextSwitchTo(w->cxt);

    if (node->layout == CHC_COL_NULLABLE && node->nullable.inner->layout == layout) {
        uint8_t b = 0;

        pgch_buf_append(&node->nullable.null_map, &b, 1);
        node = node->nullable.inner;
    }
    if (node->layout != layout) {
        pgch_errorf(
            ERRCODE_DATATYPE_MISMATCH, "%s value into %s", what, pgch__col_desc(w, col)
        );
    }
    pgch__cursor_step(w);
    if (w->cursor_len == w->cursor_cap) {
        w->cursor_cap = w->cursor_cap ? w->cursor_cap * 2 : 4;
        w->cursor = w->cursor ? repalloc(w->cursor, w->cursor_cap * sizeof(pgch__frame))
                              : palloc(w->cursor_cap * sizeof(pgch__frame));
    }
    if (w->cursor_len == 0) {
        w->cursor_col = col;
    }
    w->cursor[w->cursor_len].node  = node;
    w->cursor[w->cursor_len].child = 0;
    w->cursor_len++;
    MemoryContextSwitchTo(old);
}

static pgch__frame*
pgch__cursor_pop(pgch_writer* w, chc_col_kind layout) {
    if (w->cursor_len == 0) {
        return NULL;
    }
    pgch__frame* f = &w->cursor[w->cursor_len - 1];
    if (f->node->layout != layout) {
        pgch_error(ERRCODE_FDW_ERROR, "mismatched Array and Tuple nesting");
    }
    w->cursor_len--;
    return f;
}

void
pgch_array_begin(pgch_writer* w, size_t col) {
    pgch__cursor_push(w, col, CHC_COL_ARRAY);
}

void
pgch_array_end(pgch_writer* w) {
    pgch__frame* f = pgch__cursor_pop(w, CHC_COL_ARRAY);

    if (!f) {
        return;
    }
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__offs_push(&f->node->array.offs, pgch__node_rows(f->node->array.values));
    MemoryContextSwitchTo(old);
}

void
pgch_tuple_begin(pgch_writer* w, size_t col) {
    pgch__cursor_push(w, col, CHC_COL_TUPLE);
}

void
pgch_tuple_end(pgch_writer* w) {
    pgch__frame* f = pgch__cursor_pop(w, CHC_COL_TUPLE);

    if (f && f->child != f->node->tuple.arity) {
        pgch_errorf(
            ERRCODE_FDW_ERROR,
            "Tuple took %zu of %zu values",
            f->child,
            f->node->tuple.arity
        );
    }
}

bool
pgch_nest_active(const pgch_writer* w) {
    return w && w->cursor_len > 0;
}

chc_kind
pgch_column_kind(const pgch_writer* w, size_t col) {
    if (!w->cursor_len && col >= w->ncols) {
        return CHC_VOID;
    }

    const pgch__node* node = pgch__cursor_node(w, col);

    if (node->layout == CHC_COL_NULLABLE) {
        node = node->nullable.inner;
    }
    /* LowCardinality(String) takes the values a String column takes */
    return node->layout == CHC_COL_LOW_CARDINALITY ? CHC_STRING : node->kind;
}

uint32_t
pgch_column_datetime64_scale(const pgch_writer* w, size_t col) {
    if (!w->cursor_len && col >= w->ncols) {
        return 0;
    }
    const pgch__node* node = pgch__cursor_node(w, col);

    for (;;) {
        if (node->layout == CHC_COL_NULLABLE) {
            node = node->nullable.inner;
        } else if (node->layout == CHC_COL_ARRAY) {
            node = node->array.values;
        } else {
            break;
        }
    }
    return node->layout == CHC_COL_FIXED ? node->fixed.dt64_scale : 0;
}

static void
pgch__append_null_array(pgch_writer* w, size_t col) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__cursor_node(w, col);
    uint8_t b         = 1;
    bool nullable     = w->null_array == PGCH_NULL_ARRAY_EMPTY;

    pgch__cursor_step(w);
    while (node->layout == CHC_COL_NULLABLE) {
        pgch_buf_append(&node->nullable.null_map, &b, 1);
        nullable = true;
        node     = node->nullable.inner;
    }
    if (!nullable || node->layout != CHC_COL_ARRAY) {
        pgch__null_violation(w, col);
    }
    pgch__offs_push(&node->array.offs, pgch__node_rows(node->array.values));
    MemoryContextSwitchTo(old);
}

/* Fill the Float64 leaves left to right, a nested Point taking two values */
static const double*
pgch__append_axes(
    pgch_writer* w,
    size_t col,
    pgch__node* node,
    const double* vals,
    const double* end
) {
    for (size_t i = 0; i < node->tuple.arity; i++) {
        pgch__node* child = node->tuple.children[i];

        if (child->layout == CHC_COL_TUPLE) {
            vals = pgch__append_axes(w, col, child, vals, end);
            continue;
        }
        if (child->layout != CHC_COL_FIXED || child->fixed.elem_size != 8 ||
            child->kind != CHC_FLOAT64 || vals == end) {
            pgch_errorf(
                ERRCODE_DATATYPE_MISMATCH, "coordinates into %s", pgch__col_desc(w, col)
            );
        }
        uint64_t bits;

        memcpy(&bits, vals++, sizeof bits);
        bits = PGCH__LE64(bits);
        pgch_buf_append(&child->fixed.data, &bits, sizeof bits);
    }
    return vals;
}

static void
pgch__append_doubles(
    pgch_writer* w,
    size_t col,
    const double* vals,
    size_t n,
    bool isnull
) {
    MemoryContext old = MemoryContextSwitchTo(w->cxt);
    pgch__node* node  = pgch__resolve_leaf(w, col, isnull);

    if (node->layout != CHC_COL_TUPLE ||
        pgch__append_axes(w, col, node, vals, vals + n) != vals + n) {
        pgch_errorf(
            ERRCODE_DATATYPE_MISMATCH, "coordinates into %s", pgch__col_desc(w, col)
        );
    }
    MemoryContextSwitchTo(old);
}

/* ---- Datum appends -------------------------------------------------- */

/* Ring and LineString are Array(Point), each point a Tuple(Float64, Float64) */
static void
pgch__append_points(
    pgch_writer* w,
    size_t col,
    const Point* pts,
    int npts,
    bool close
) {
    /* Repeat first point to carry closed, as a closed GeoJSON line */
    int n = close && npts ? npts + 1 : npts;

    pgch_array_begin(w, col);
    for (int i = 0; i < n; i++) {
        const Point* p = &pts[i == npts ? 0 : i];
        double xy[2]   = { p->x, p->y };

        pgch__append_doubles(w, 0, xy, lengthof(xy), false);
    }
    pgch_array_end(w);
}

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

/* Return true while appending a Tuple field */
static inline bool
pgch__in_tuple(const pgch_writer* w) {
    return w->cursor_len && w->cursor[w->cursor_len - 1].node->layout == CHC_COL_TUPLE;
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

            /* Convert text array items with each Tuple field's input function
             * Array items share one PostgreSQL type, but Tuple fields can differ */
            if (to != TEXTOID && !(from == TEXTOID && pgch__in_tuple(w))) {
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

/*
 * Arrays, Maps, Polygons, and multi-geometries use PostgreSQL arrays
 * Maps contain key-value Tuples
 */
static inline bool
pgch__kind_takes_array(chc_kind kind) {
    return kind == CHC_ARRAY || kind == CHC_MAP || kind == CHC_POLYGON ||
           kind == CHC_MULTI_POLYGON || kind == CHC_MULTI_LINE_STRING;
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
    if (pgch__kind_takes_array(kind) && isnull) {
        pgch__append_null_array(w, col);
        return;
    }
    /* Read Tuple fields from array items, including key-value pairs in Maps */
    if ((pgch__kind_takes_array(kind) || kind == CHC_TUPLE) && !isnull &&
        valtype != ANYARRAYOID) {
        pgch__node* node  = pgch__cursor_node(w, col);
        pgch__arrmeta* am = node->arrmeta;

        if (!am) {
            am = node->arrmeta = MemoryContextAllocZero(w->cxt, sizeof(*am));
        }
        ArrayMetaState* ms = &am->meta;
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

    switch (valtype) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
#if PG_VERSION_NUM >= 190000
    case OID8OID:
#endif
    case XID8OID: {
        int64_t v = 0;

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
        /* Integer widths follow the column, since one Datum type feeds them all */
        switch (kind) {
        case CHC_INT8:
        case CHC_UINT8:
        case CHC_BOOL:
            pgch__append_i8(w, col, v, isnull);
            return;
        case CHC_INT16:
        case CHC_UINT16:
            pgch__append_i16(w, col, v, isnull);
            return;
        case CHC_INT32:
        case CHC_UINT32:
            pgch__append_i32(w, col, v, isnull);
            return;
        case CHC_INT64:
        case CHC_UINT64:
            pgch__append_i64(w, col, v, isnull);
            return;
        default:
            goto type_mismatch;
        }
    }
    case BOOLOID:
        if (kind != CHC_BOOL && kind != CHC_UINT8) {
            goto type_mismatch;
        }
        pgch__append_i8(w, col, DatumGetBool(val), isnull);
        return;
    case FLOAT4OID:
        if (kind == CHC_BFLOAT16) {
            pgch__append_bf16(w, col, DatumGetFloat4(val), isnull);
            return;
        }
        if (kind != CHC_FLOAT32) {
            goto type_mismatch;
        }
        pgch__append_f32(w, col, DatumGetFloat4(val), isnull);
        return;
    case FLOAT8OID:
        if (kind != CHC_FLOAT64) {
            goto type_mismatch;
        }
        pgch__append_f64(w, col, DatumGetFloat8(val), isnull);
        return;
    case NUMERICOID: {
        char* s = NULL;

        if (!pgch__kind_maps_to_numeric(kind)) {
            goto type_mismatch;
        }
        if (!isnull) {
            Numeric num = DatumGetNumeric(val);

            s = numeric_normalize(num);
            if ((Pointer)num != DatumGetPointer(val)) {
                pfree(num);
            }
        }
        pgch__append_number(w, col, s, isnull);
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
        pgch__append_bytes(w, col, p, len, isnull);
        if (string && (Pointer)string != DatumGetPointer(val)) {
            pfree(string);
        }
        return;
    }
    case DATEOID: {
        int64_t days = isnull ? 0 : (int64_t)DatumGetDateADT(val) + PGCH__DATE_OFFSET;

        if (kind == CHC_DATE) {
            pgch__append_i16(w, col, days, isnull);
        } else if (kind == CHC_DATE32) {
            pgch__append_i32(w, col, days, isnull);
        } else {
            goto type_mismatch;
        }
        return;
    }
    case TIMEOID: {
        int64_t usec = isnull ? 0 : (int64_t)DatumGetTimeADT(val);

        switch (kind) {
        /* ClickHouse Time supports signed values up to 999 hours */
        case CHC_TIME:
            pgch__append_i32(w, col, usec / USECS_PER_SEC, isnull);
            return;
        case CHC_TIME64: {
            int64 power = pgch_pow10[pgch_column_datetime64_scale(w, col)];

            pgch__append_i64(
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
    case INTERVALOID:
        if (kind != CHC_INTERVAL) {
            goto type_mismatch;
        }
        pgch__append_interval(w, col, isnull ? NULL : DatumGetIntervalP(val), isnull);
        return;
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

            pgch__append_i32(w, col, seconds, isnull);
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
            pgch__append_i64(w, col, raw, isnull);
        } break;
        default:
            goto type_mismatch;
        }
        return;
    }
    case ANYARRAYOID: {
        if (kind != CHC_TUPLE && !pgch__kind_takes_array(kind)) {
            goto type_mismatch;
        }
        if (isnull) {
            /* Tuple values cannot be NULL, empty Array or Map represents NULL input */
            if (kind == CHC_TUPLE) {
                pgch__null_violation(w, col);
            }
            pgch__append_null_array(w, col);
            return;
        }

        pgch_array* arr   = (pgch_array*)DatumGetPointer(val);
        Oid child_valtype = (arr->ndim > 1) ? ANYARRAYOID : arr->item_type;

        /* Read Tuple fields, including key-value pairs in Maps */
        if (kind == CHC_TUPLE) {
            pgch_tuple_begin(w, col);
            for (size_t i = 0; i < arr->len; i++) {
                pgch__append_one(
                    w,
                    0,
                    pgch_column_kind(w, 0),
                    arr->datums[i],
                    child_valtype,
                    arr->nulls[i]
                );
            }
            pgch_tuple_end(w);
            return;
        }

        pgch_array_begin(w, col);

        chc_kind item_kind = pgch_column_kind(w, col);
        for (size_t i = 0; i < arr->len; i++) {
            pgch__append_one(
                w, 0, item_kind, arr->datums[i], child_valtype, arr->nulls[i]
            );
        }

        pgch_array_end(w);
        return;
    }
    case UUIDOID: {
        uint8_t wire[16] = {};

        if (kind != CHC_UUID) {
            goto type_mismatch;
        }
        if (!isnull) {
            /* PostgreSQL and ClickHouse use opposite byte order for each UUID half */
            const uint8_t* bytes = DatumGetUUIDP(val)->data;
            uint64_t hi, lo;

            memcpy(&hi, bytes, 8);
            memcpy(&lo, bytes + 8, 8);
            hi = pg_bswap64(hi);
            lo = pg_bswap64(lo);
            memcpy(wire, &hi, 8);
            memcpy(wire + 8, &lo, 8);
        }
        pgch__append_raw(w, col, wire, sizeof wire, isnull);
        return;
    }
    case POINTOID: {
        double axes[2] = {};

        if (kind != CHC_POINT) {
            goto type_mismatch;
        }
        if (!isnull) {
            Point* point = DatumGetPointP(val);

            axes[0] = point->x;
            axes[1] = point->y;
        }
        pgch__append_doubles(w, col, axes, lengthof(axes), isnull);
        return;
    }
    case LSEGOID: {
        if (kind != CHC_LINE_STRING && kind != CHC_RING) {
            goto type_mismatch;
        }
        if (isnull) {
            pgch__append_null_array(w, col);
            return;
        }
        pgch__append_points(w, col, DatumGetLsegP(val)->p, 2, false);
        return;
    }
    case PATHOID: {
        if (kind != CHC_LINE_STRING && kind != CHC_RING) {
            goto type_mismatch;
        }
        if (isnull) {
            pgch__append_null_array(w, col);
            return;
        }
        PATH* path = DatumGetPathP(val);
        pgch__append_points(w, col, path->p, path->npts, path->closed);
        if ((Pointer)path != DatumGetPointer(val)) {
            pfree(path);
        }
        return;
    }
    case POLYGONOID: {
        if (kind != CHC_RING && kind != CHC_LINE_STRING) {
            goto type_mismatch;
        }
        if (isnull) {
            pgch__append_null_array(w, col);
            return;
        }
        POLYGON* poly = DatumGetPolygonP(val);
        pgch__append_points(w, col, poly->p, poly->npts, false);
        if ((Pointer)poly != DatumGetPointer(val)) {
            pfree(poly);
        }
        return;
    }
    case BOXOID: {
        double axes[4] = {};

        if (kind != CHC_TUPLE) {
            goto type_mismatch;
        }
        if (!isnull) {
            BOX* box = DatumGetBoxP(val);

            axes[0] = box->high.x;
            axes[1] = box->high.y;
            axes[2] = box->low.x;
            axes[3] = box->low.y;
        }
        pgch__append_doubles(w, col, axes, lengthof(axes), isnull);
        return;
    }
    case CIRCLEOID: {
        double axes[3] = {};

        if (kind != CHC_TUPLE) {
            goto type_mismatch;
        }
        if (!isnull) {
            CIRCLE* circle = DatumGetCircleP(val);

            axes[0] = circle->center.x;
            axes[1] = circle->center.y;
            axes[2] = circle->radius;
        }
        pgch__append_doubles(w, col, axes, lengthof(axes), isnull);
        return;
    }
    case LINEOID: {
        double axes[3] = {};

        if (kind != CHC_TUPLE) {
            goto type_mismatch;
        }
        if (!isnull) {
            LINE* line = DatumGetLineP(val);

            axes[0] = line->A;
            axes[1] = line->B;
            axes[2] = line->C;
        }
        pgch__append_doubles(w, col, axes, lengthof(axes), isnull);
        return;
    }
    /* ClickHouse stores IPv4 in host order and IPv6 in network order */
    case INETOID: {
        const uint8_t* addr = NULL;

        if (kind != CHC_IPV4 && kind != CHC_IPV6) {
            goto type_mismatch;
        }
        if (!isnull) {
            inet* ipa    = DatumGetInetPP(val);
            int expected = kind == CHC_IPV4 ? PGSQL_AF_INET : PGSQL_AF_INET6;

            if (ip_family(ipa) != expected) {
                pgch_errorf(
                    ERRCODE_DATATYPE_MISMATCH,
                    "inet family mismatch for %s",
                    pgch__col_desc(w, col)
                );
            }
            addr = ip_addr(ipa);
        }
        if (kind == CHC_IPV4) {
            uint32_t be = 0;

            if (addr) {
                memcpy(&be, addr, 4);
            }
            pgch__append_i32(w, col, pg_ntoh32(be), isnull);
        } else {
            pgch__append_raw(w, col, addr, 16, isnull);
        }
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
        pgch__append_bytes(w, col, p, len, isnull);
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
    /* Map entry arrays have no PostgreSQL type to convert */
    if (OidIsValid(node->target) && node->target != valtype) {
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

        if (nullable && node->lc.null_map.data[i]) {
            keys[i] = 0;
            continue;
        }

        bool found;
        pgch_lcd_entry* entry = pgch_lcd_insert(ht, k, &found);
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

        dict_data = MemoryContextAllocHuge(CurrentMemoryContext, data_len);
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
    switch (n->layout) {
    case CHC_COL_FIXED:
        return pgch__col_node(
            chc_build_fixed(n->fixed.data.data, n->fixed.elem_size, pgch__node_rows(n))
        );
    case CHC_COL_STRING:
        return pgch__col_node(chc_build_string(
            pgch__offs_data(&n->str.offs),
            n->str.data.data,
            pgch__offs_len(&n->str.offs)
        ));
    case CHC_COL_NULLABLE:
        return pgch__col_node(chc_build_nullable(
            n->nullable.null_map.data, pgch__finalize_node(n->nullable.inner)
        ));
    case CHC_COL_ARRAY:
        return pgch__col_node(chc_build_array(
            pgch__offs_data(&n->array.offs),
            pgch__offs_len(&n->array.offs),
            pgch__finalize_node(n->array.values)
        ));
    case CHC_COL_TUPLE: {
        chc_column** children = palloc(n->tuple.arity * sizeof(*children));

        for (size_t i = 0; i < n->tuple.arity; i++) {
            children[i] = pgch__finalize_node(n->tuple.children[i]);
        }
        return pgch__col_node(chc_build_tuple(children, n->tuple.arity));
    }
    case CHC_COL_LOW_CARDINALITY: {
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
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

static size_t
pgch__node_bytes(const pgch__node* n) {
    switch (n->layout) {
    case CHC_COL_FIXED:
        return n->fixed.data.len;
    case CHC_COL_STRING:
        return n->str.data.len + n->str.offs.len;
    case CHC_COL_NULLABLE:
        return n->nullable.null_map.len + pgch__node_bytes(n->nullable.inner);
    case CHC_COL_ARRAY:
        return n->array.offs.len + pgch__node_bytes(n->array.values);
    case CHC_COL_TUPLE: {
        size_t total = 0;

        for (size_t i = 0; i < n->tuple.arity; i++) {
            total += pgch__node_bytes(n->tuple.children[i]);
        }
        return total;
    }
    case CHC_COL_LOW_CARDINALITY:
        return n->lc.data.len + n->lc.offs.len + n->lc.null_map.len;
    case CHC_COL_NOTHING:
        break;
    }
    pg_unreachable();
}

static void
pgch__reset_node(pgch__node* n) {
    switch (n->layout) {
    case CHC_COL_FIXED:
        pgch_buf_reset(&n->fixed.data);
        return;
    case CHC_COL_STRING:
        pgch_buf_reset(&n->str.data);
        pgch_buf_reset(&n->str.offs);
        return;
    case CHC_COL_NULLABLE:
        pgch_buf_reset(&n->nullable.null_map);
        pgch__reset_node(n->nullable.inner);
        return;
    case CHC_COL_ARRAY:
        pgch_buf_reset(&n->array.offs);
        pgch__reset_node(n->array.values);
        return;
    case CHC_COL_TUPLE:
        for (size_t i = 0; i < n->tuple.arity; i++) {
            pgch__reset_node(n->tuple.children[i]);
        }
        return;
    case CHC_COL_LOW_CARDINALITY:
        pgch_buf_reset(&n->lc.data);
        pgch_buf_reset(&n->lc.offs);
        pgch_buf_reset(&n->lc.null_map);
        return;
    case CHC_COL_NOTHING:
        break;
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
    /* Close every array context before building block */
    Assert(w->cursor_len == 0);

    if (w->bcxt) {
        MemoryContextDelete(w->bcxt);
    }
    w->bcxt = AllocSetContextCreate(w->cxt, "pgch block", ALLOCSET_DEFAULT_SIZES);

    MemoryContext old    = MemoryContextSwitchTo(w->bcxt);
    chc_block_col* bcols = w->ncols ? palloc(w->ncols * sizeof(*bcols)) : NULL;
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
    w->generation++;
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
        pgch_raise(&err, ERRCODE_FDW_ERROR, "block write: ", NULL);
    }
    pgch_writer_reset(w);
}

#endif /* PGCH_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* PG_CLICKHOUSE_ENCODE_H */
