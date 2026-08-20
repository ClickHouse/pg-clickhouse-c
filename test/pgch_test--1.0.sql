\echo Use "CREATE EXTENSION pgch_test" to load this file. \quit

CREATE FUNCTION pgch_encode(ch_type text, val anyelement) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_encode_rows(ch_type text, vals anyarray) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

-- Encode one row of pairs, fields counting values written per pair
CREATE FUNCTION pgch_encode_pairs(ch_type text, keys text[], vals bigint[],
                                  fields int DEFAULT 2,
                                  nest bool DEFAULT false) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_decode(data bytea) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Pass NULL::type to select target type
CREATE FUNCTION pgch_decode_as(data bytea, target anyelement) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

-- Prepare conversion from ClickHouse column type
CREATE FUNCTION pgch_decode_typed(data bytea, target anyelement) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_pgtype(ch_type text) RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Describe PostgreSQL column mapped from ClickHouse declaration
CREATE FUNCTION pgch_pgcolumn(ch_type text, OUT type text, OUT ndims int,
                              OUT nullable bool, OUT truncated bool,
                              OUT is_column bool)
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- ClickHouse to PostgreSQL type table of README
CREATE FUNCTION pgch_type_table() RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Declarations the type table leaves out, with the reason
CREATE FUNCTION pgch_type_omitted() RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Split tab separated rows into columns for psql to lay out
CREATE FUNCTION pgch_rows(lines text[]) RETURNS SETOF text[]
    LANGUAGE sql IMMUTABLE STRICT AS $$
    SELECT string_to_array(line, E'\t')
        FROM unnest(lines) WITH ORDINALITY u(line, ord) ORDER BY ord $$;

CREATE FUNCTION pgch_native_settings() RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Decode bytes delivered in fixed-size chunks
CREATE FUNCTION pgch_decode_chunks(data bytea, chunk int) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_chtype(decl text, notnull bool DEFAULT false,
                            json_as_json bool DEFAULT false,
                            low_cardinality bool DEFAULT false,
                            numeric_as_string bool DEFAULT false) RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_structure(rel regclass,
                               json_as_json bool DEFAULT false,
                               low_cardinality bool DEFAULT false,
                               numeric_as_string bool DEFAULT false) RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Round-trip relation through generated structure
CREATE FUNCTION pgch_table_roundtrip(rel regclass,
                                     json_as_json bool DEFAULT false,
                                     low_cardinality bool DEFAULT false,
                                     numeric_as_string bool DEFAULT false,
                                     null_array_empty bool DEFAULT false)
    RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Build one-column Native block from supplied payload
CREATE FUNCTION pgch_block(ch_type text, nrows int, payload bytea) RETURNS bytea
    LANGUAGE sql IMMUTABLE AS $$
    SELECT '\x01'::bytea || set_byte('\x00'::bytea, 0, nrows)
        || '\x01'::bytea || convert_to('c', 'UTF8')
        || set_byte('\x00'::bytea, 0, octet_length(convert_to(ch_type, 'UTF8')))
        || convert_to(ch_type, 'UTF8') || payload $$;

CREATE FUNCTION pgch_roundtrip(ch_type text, val anyelement) RETURNS text
    LANGUAGE sql AS $$ SELECT (pgch_decode(pgch_encode($1, $2)))[1] $$;

-- Encode with ClickHouse type, decode back into input type
CREATE FUNCTION pgch_roundtrip_as(ch_type text, val anyelement) RETURNS text
    LANGUAGE sql AS $$ SELECT (pgch_decode_as(pgch_encode($1, $2), $2))[1] $$;

CREATE FUNCTION pgch_roundtrip_rows(ch_type text, vals anyarray) RETURNS text[]
    LANGUAGE sql AS $$ SELECT pgch_decode(pgch_encode_rows($1, $2)) $$;
