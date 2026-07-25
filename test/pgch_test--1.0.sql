\echo Use "CREATE EXTENSION pgch_test" to load this file. \quit

CREATE FUNCTION pgch_encode(ch_type text, val anyelement) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_encode_rows(ch_type text, vals anyarray) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_decode(data bytea) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- target is only read for its type; pass NULL::sometype
CREATE FUNCTION pgch_decode_as(data bytea, target anyelement) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

-- as pgch_decode_as, with conversion state built from the column's CH type
CREATE FUNCTION pgch_decode_typed(data bytea, target anyelement) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_pgtype(ch_type text) RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_native_settings() RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- Same bytes, handed to the reader in `chunk`-byte pieces.
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

-- Every row of rel through its own declared structure and back. nonfinite is
-- the pgch_nonfinite enum: 0 keep, 1 null, 2 zero.
CREATE FUNCTION pgch_table_roundtrip(rel regclass,
                                     json_as_json bool DEFAULT false,
                                     low_cardinality bool DEFAULT false,
                                     numeric_as_string bool DEFAULT false,
                                     null_array_empty bool DEFAULT false,
                                     nonfinite int DEFAULT 0)
    RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

-- One-column Native block from hand-written payload bytes, for the shapes the
-- encoder cannot produce. Counts and the name length stay one-byte varints.
CREATE FUNCTION pgch_block(ch_type text, nrows int, payload bytea) RETURNS bytea
    LANGUAGE sql IMMUTABLE AS $$
    SELECT '\x01'::bytea || set_byte('\x00'::bytea, 0, nrows)
        || '\x01'::bytea || convert_to('c', 'UTF8')
        || set_byte('\x00'::bytea, 0, octet_length(convert_to(ch_type, 'UTF8')))
        || convert_to(ch_type, 'UTF8') || payload $$;

CREATE FUNCTION pgch_roundtrip(ch_type text, val anyelement) RETURNS text
    LANGUAGE sql AS $$ SELECT (pgch_decode(pgch_encode($1, $2)))[1] $$;

-- Both directions: out through the CH type, back into val's own PG type.
CREATE FUNCTION pgch_roundtrip_as(ch_type text, val anyelement) RETURNS text
    LANGUAGE sql AS $$ SELECT (pgch_decode_as(pgch_encode($1, $2), $2))[1] $$;

CREATE FUNCTION pgch_roundtrip_rows(ch_type text, vals anyarray) RETURNS text[]
    LANGUAGE sql AS $$ SELECT pgch_decode(pgch_encode_rows($1, $2)) $$;
