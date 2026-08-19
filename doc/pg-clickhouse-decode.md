# pg-clickhouse-decode.h

Decode ClickHouse Native blocks into PostgreSQL Datums. Include
[pg-clickhouse.h](pg-clickhouse.md), then this header.

Define `PGCH_IMPLEMENTATION` in exactly one translation unit. Define
`CHC_IMPLEMENTATION` in same translation unit when using chunk source API.

## Decode one value

```c
Datum pgch_read_value(const chc_column *col, const chc_type *type,
                      uint64_t row, Oid *valtype, bool *is_null);
```

Pass wire column, matching ClickHouse type, and row index. Function returns
PostgreSQL Datum, writes returned type OID to `*valtype`, and sets
`*is_null`.

Set incoming `*valtype` to request optional mappings:

- Set `JSONOID` for `JSON` or `Object` to preserve document text as
  PostgreSQL `json`; default is `JSONBOID`
- On PostgreSQL 19 or later, set `OID8OID` for `UInt64` to support full
  unsigned range; default is `INT8OID` and values above `2^63 - 1` raise

Returned variable-length values are allocated with `palloc`. Unsupported types
raise `ERRCODE_FDW_INVALID_DATA_TYPE`.

Geo types decode to geometric Datums: `Point` to `point`, `Ring` to `polygon`,
`LineString` to `path`, closed when its first point repeats, and `Polygon`,
`MultiLineString` and `MultiPolygon` to `pgch_array` over those. An empty ring
or line decodes as NULL, PostgreSQL having no pointless polygon or path.

Function trusts array offsets and LowCardinality keys, so pass columns that
passed `chc_column_validate`. Most consumers should use `pgch_reader` instead of
calling `pgch_read_value` directly.

## Supply decoded blocks

```c
typedef struct pgch_block_source {
    void *ud;
    const chc_block *(*next_block)(void *ud);
    const char       *(*error)(void *ud);
} pgch_block_source;
```

`next_block` returns one block per call. Returning non-NULL transfers ownership
to reader. Reader destroys block with `pgch_alloc`, so source must read it with
same allocator.

Return `NULL` for end of stream. On failure, return `NULL` and make `error`
return message. `error` must be callable before first block, after every
`next_block`, and at end.

Reader runs `chc_column_validate` on every column of every block, so source
need not repeat it. Violation sets `reader.error` and ends stream.

Source still owns transport-specific recovery. Validate in source when invalid
block must change transport state, for example when it makes connection unsafe
to reuse:

```c
for (size_t i = 0; i < chc_block_n_columns(block); i++) {
    chc_err err = {};

    if (chc_column_validate(chc_block_column(block, i), &err) != CHC_OK)
        report_invalid_block(&err);
}
```

## Supply Native byte chunks

```c
typedef struct pgch_chunk_source {
    void *ud;
    bool (*next_chunk)(void *ud, const void **p, size_t *n, char **error);
    bool (*cancelled)(void *ud);
} pgch_chunk_source;

void pgch_reader_init_chunks(pgch_reader *r,
                             const pgch_chunk_source *src,
                             const chc_block_opts *opts);
```

Use chunk source when transport provides Native bytes instead of decoded
blocks. `next_chunk` returns pointer and length. Keep bytes valid until next
`next_chunk` call.

- Set `*n = 0` and return true for end of stream
- Set `*error` and return false for source failure
- Set `cancelled` to `NULL` when cancellation polling is not needed

Ending between blocks is clean end. Ending inside block produces truncation
error. Pass `NULL` options to use `pgch_block_opts_local`.

Blocks use memory context active when `pgch_reader_init_chunks` is called. Keep
this context alive until reader is finished. Callers can safely use a per-row
context while reading rows.

Chunk source API is available when implementation translation unit defines
both `PGCH_IMPLEMENTATION` and `CHC_IMPLEMENTATION`.

## Read rows

```c
typedef struct pgch_reader {
    pgch_block_source src;

    Oid    *coltypes;
    char  **colshapes;
    Datum  *values;
    bool   *nulls;

    size_t  ncols;
    size_t  row;
    const chc_block *cur;
    MemoryContext    cxt;
    char   *error;
    bool    done;
} pgch_reader;

void   pgch_reader_init(pgch_reader *r, const pgch_block_source *src);
bool   pgch_reader_next(pgch_reader *r);
size_t pgch_reader_columns(const pgch_reader *r);
void   pgch_reader_free(pgch_reader *r);
```

Initialize in memory context that should own reader state and error text.
Reader itself may be stack allocated.

Initialization reads first block and sets `ncols` and `coltypes`. Empty stream,
zero columns, unsupported schema, or source failure leaves reader done. Check
`reader.error` after initialization.

Override optional column mappings before first row:

```c
pgch_reader_init(&reader, &source);

for (size_t i = 0; i < reader.ncols; i++) {
    if (reader.coltypes[i] == JSONBOID && want_json)
        reader.coltypes[i] = JSONOID;
}
```

Call `pgch_reader_next` until false. Each successful call fills `values` and
`nulls`, both `ncols` long. Consume row before next call. Reader advances
across blocks and presents one continuous row stream.

```c
while (pgch_reader_next(&reader)) {
    consume_row(reader.values, reader.nulls, reader.coltypes, reader.ncols);
}

if (reader.error)
    ereport(ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("%s", reader.error));
```

Reader rejects unsupported column types, schema changes that would alter
returned Datum shape, and columns failing `chc_column_validate`. Those failures
set `reader.error`. Clean end leaves it `NULL`.

Value conversion can still raise PostgreSQL errors, including invalid JSON or
numeric text, `UInt64` outside selected target range, and payload that
contradicts declared type.

`pgch_reader_free` releases current block and clears `reader.error`. Save error
pointer first when it must survive that call; storage remains valid until
initialization memory context is reset or deleted.

## Compare Datum shapes

```c
char *pgch_type_shape(const chc_type *type);
```

Returns palloc'd signature for decoded Datum shape. Use equality when caching
conversion state across independently managed block streams.

## Convert into target PostgreSQL types

```c
void *pgch_convert_init(Datum val, Oid intype, Oid outtype, int32 outtypmod);
void *pgch_convert_init_type(const chc_type *in, Oid outtype, int32 outtypmod);
void *pgch_reader_convert_init(const pgch_reader *r,
                               size_t col, Oid outtype, int32 outtypmod);

Datum pgch_convert(void *state, Datum val);
void  pgch_convert_free(void *state);
```

Conversion supports:

- `pgch_array` to PostgreSQL arrays, nested arrays must share dimensions
  because PostgreSQL arrays are rectangular
- `pgch_tuple` to PostgreSQL records and named composite types
- `pgch_tuple` of coordinates to `box`, `circle` and `line`, and a decoded
  `path` or `polygon` of two points to `lseg`, none of which PostgreSQL casts
- `Map` as an array of two-field composites, so a target composite array with
  matching key and value types receives it
- ClickHouse `String` values through PostgreSQL target input function
- Explicit PostgreSQL casts between scalar types
- Per-element conversion when source and target array element types differ, or
  when the target array carries a type modifier

Pass target `atttypmod`, or `-1` when the target carries none. Length and
precision then apply as PostgreSQL applies them on assignment: `char(n)` pads,
`varchar(n)` rejects overlong values, `numeric(p,s)` rounds and
`timestamp(n)` truncates. A domain supplies its own typmod. An array column's
typmod belongs to its elements, so pass it unchanged.

When the target is a domain, conversion first produces the domain's base type,
then checks the domain constraints. The same applies to elements in an array of
domains. NULL rows are not converted, so callers must enforce a domain's NOT
NULL constraint

Prefer `pgch_reader_convert_init` when reader and target tuple descriptor are
available. It prepares conversion from schema before reading rows, including
columns whose first or every value is NULL:

```c
void **states = palloc0(reader.ncols * sizeof(*states));

for (size_t i = 0; i < reader.ncols; i++)
    states[i] = pgch_reader_convert_init(&reader, i,
                                         TupleDescAttr(desc, i)->atttypid,
                                         TupleDescAttr(desc, i)->atttypmod);
```

`pgch_convert_init_type` provides same behavior for standalone ClickHouse
type.

Use `pgch_convert_init` when only representative value is available. Arrays
and tuples require non-NULL representative value because shape comes from
intermediate representation.

All initialization functions allocate state in `CurrentMemoryContext`. Build
state in context that outlives row loop. They return `NULL` when no conversion
is required. `pgch_convert` accepts `NULL` state and returns input unchanged.
`pgch_convert_free` releases top-level state; enclosing memory context owns
associated allocations.

## Fill target row

```c
void pgch_reader_fill(const pgch_reader *r, void **states,
                      Datum *values, bool *nulls);
void pgch_reader_fill_map(const pgch_reader *r, void **states,
                          const int *dest, Datum *values, bool *nulls);
```

Convert current reader row into caller arrays. `values`, `nulls`, and optional
`states` must each hold `r->ncols` entries. Pass `NULL` for `states`, or use
`NULL` entries for columns requiring no conversion.

```c
while (pgch_reader_next(&reader)) {
    pgch_reader_fill(&reader, states, values, nulls);
    slot = heap_form_tuple(desc, values, nulls);
}
```

`pgch_reader_fill_map` writes column `i` to `dest[i]` instead, for a target
holding attributes no stream column feeds. Positions outside `dest` keep
whatever the caller left there, so fill them, or preset their `nulls` entry:

```c
memset(slot->tts_isnull, true, desc->natts * sizeof(bool));
pgch_reader_fill_map(&reader, states, attnums,
                     slot->tts_values, slot->tts_isnull);
```

## Render values as text

```c
char *pgch_value_to_cstring(Oid coltype, Datum value);
```

Return palloc'd text representation for decoded value. Function also handles
`pgch_array` and `pgch_tuple` intermediate representations. Use Datum path
instead when String or FixedString may contain NUL bytes.

## Complete reader example

```c
pgch_reader reader;

pgch_reader_init(&reader, &source);
if (reader.error)
    ereport(ERROR, errmsg("%s", reader.error));

void **states = palloc0(reader.ncols * sizeof(*states));
Datum *values = palloc(reader.ncols * sizeof(*values));
bool *nulls = palloc(reader.ncols * sizeof(*nulls));

for (size_t i = 0; i < reader.ncols; i++)
    states[i] = pgch_reader_convert_init(&reader, i,
                                         TupleDescAttr(desc, i)->atttypid,
                                         TupleDescAttr(desc, i)->atttypmod);

while (pgch_reader_next(&reader)) {
    pgch_reader_fill(&reader, states, values, nulls);
    consume_tuple(heap_form_tuple(desc, values, nulls));
}

if (reader.error)
    ereport(ERROR, errmsg("%s", reader.error));

pgch_reader_free(&reader);
```
