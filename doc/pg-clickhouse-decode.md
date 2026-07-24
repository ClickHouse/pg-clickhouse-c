# pg-clickhouse-decode.h

Native block to PostgreSQL `Datum`. A row cursor over a block stream you
supply, plus the conversion step that turns Array and Tuple carriers into real
PG values.

Depends on [pg-clickhouse.h](pg-clickhouse.md). Exactly one TU defines
`PGCH_IMPLEMENTATION` before including.

## Reading a value

```c
Datum pgch_read_value(const chc_column *col, const chc_type *type,
                      uint64_t row, Oid *valtype, bool *is_null);
```

One value out of a wire-shaped `chc_column`. No pre-pass, no per-column state:
`Nullable` strip, `LowCardinality` key deref, decimal-to-`numeric`,
IPv4/IPv6-to-`inet`, UUID byteswap and enum name lookup all happen inline at
read time, which is why a scan pays only for the columns it touches.

`*valtype` is in-out. It arrives holding your preferred OID and leaves holding
the OID of the returned `Datum`. Only `JSON` / `Object` honor the incoming
value: pass `JSONOID` and the document text goes through `json_in`, keeping
ClickHouse's verbatim formatting, instead of `jsonb_in` normalizing it. Every
other kind overwrites `*valtype` with the canonical mapping.

Raises `ERRCODE_FDW_INVALID_DATA_TYPE` on an unmapped kind and
`ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE` on a `UInt64` above `2^63 - 1`.

Decoded values are freshly `palloc`'d, never pointers into the block, so a
block can be destroyed as soon as its rows are consumed.

## Block source

```c
typedef struct pgch_block_source {
    void *ud;
    const chc_block *(*next_block)(void *ud);
    const char       *(*error)(void *ud);
} pgch_block_source;
```

The seam where transport stops. The reader never reads bytes; it asks for the
next block. `next_block` hands ownership over and the reader destroys each
block with `pgch_alloc` before asking for the next, so blocks are read with
`pgch_alloc` too.

Return NULL for end of stream. Return NULL with `error` reporting a message
for failure; `error` is also polled once before the first block, so a request
that failed before any data arrived reports cleanly. Both callbacks run inside
the reader's calls, so they may `ereport` if that suits your teardown, but a
returned error string is easier to attach context to.

## Row cursor

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

`pgch_reader_init` loads the first block, which defines the schema and may
carry zero rows, and records `CurrentMemoryContext` as `cxt` for its own state
and any error string. Allocate the reader itself wherever you like; a stack
`pgch_reader` is fine.

After init, `coltypes[i]` holds `pgch_datum_oid` of column `i`. Overwrite an
entry before the first `pgch_reader_next` to steer the one decision
`pgch_read_value` leaves open:

```c
if (r.coltypes[i] == JSONBOID && want_json)
    r.coltypes[i] = JSONOID;
```

`pgch_reader_next` fills `values` / `nulls` and returns false at end of
stream or on error. It advances blocks as needed, so a caller sees one flat row
sequence regardless of how the peer chunked it. `error` is NULL on clean end.
Decode failures are caught rather than propagated: the message lands in
`error`, `done` is set, and the reader stops, which lets the caller attach the
query text before raising.

Every block after the first is checked against the first block's column count
and per-column shape signature, where the shape is the type tree reduced to
mapped PG OIDs. A block whose columns changed shape ends the stream with an
error, so conversion state cached across blocks stays valid. Type changes that
map to the same Datum shape (`Int8` to `Int16`, adding `Nullable`) pass.

`pgch_reader_free` destroys the current block. It leaves `error` in `cxt` for
the caller to read after the fact; deleting that context frees it.

```c
char *pgch_type_shape(const chc_type *type);
```

The signature the stability check compares, exposed for callers caching their
own per-column state across blocks.

## Conversion

```c
void *pgch_convert_init(Datum val, Oid intype, Oid outtype);
Datum pgch_convert(void *state, Datum val);
void  pgch_convert_free(void *state);
```

`pgch_read_value` produces the canonical Datum for a CH type. Getting from
there to the type a caller actually wants covers three cases, all behind one
interface:

* **Carriers.** `ANYARRAYOID` builds an `ArrayType` via `construct_md_array`,
  one PG dimension per `Array` layer. `RECORDOID` builds a `HeapTuple`,
  optionally mapped onto a declared composite type or rendered as text.
* **Text input.** A CH `String` into any PG type runs the target's input
  function, which is how `interval`, `bit`, ranges and domains arrive.
* **Casts.** Anything else takes the explicit coercion pathway, or raises
  `ERRCODE_FDW_INVALID_DATA_TYPE` if there is none.

`val` must be a representative value, not just a type: array element info and
tuple field descriptors come from its shape. NULL back means no conversion is
needed, so pass the Datum through:

```c
void *cs = pgch_convert_init(r.values[i], r.coltypes[i], attr_oid);
/* ... per row ... */
out[i] = pgch_convert(cs, r.values[i]);   /* cs == NULL is a no-op */
```

State is allocated in `CurrentMemoryContext` and reusable across rows and
blocks, so build it somewhere that outlives the per-row loop. It costs syscache
lookups and a `TupleDesc`, so building it per row is measurable.

A CH array that is ragged, which PG cannot represent, falls back to formatting
the value as an array literal and running `array_in`, so the caller sees PG's
own malformed-literal error rather than a silently reshaped array.

```c
char *pgch_value_to_cstring(Oid coltype, Datum value);
```

Text rendering for callers with no target type at all, an ad-hoc query for
instance. Carriers route through `pgch_convert` first, since they have no
output function of their own. Embedded NUL bytes do not survive a cstring, so
`FixedString` padding and binary `String` payloads need the Datum path.
