\echo Use "CREATE EXTENSION pgch_test" to load this file. \quit

CREATE FUNCTION pgch_encode(ch_type text, val anyelement) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_encode_rows(ch_type text, vals anyarray) RETURNS bytea
    AS 'MODULE_PATHNAME' LANGUAGE c CALLED ON NULL INPUT;

CREATE FUNCTION pgch_decode(data bytea) RETURNS text[]
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_pgtype(ch_type text) RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE c STRICT;

CREATE FUNCTION pgch_roundtrip(ch_type text, val anyelement) RETURNS text
    LANGUAGE sql AS $$ SELECT (pgch_decode(pgch_encode($1, $2)))[1] $$;

CREATE FUNCTION pgch_roundtrip_rows(ch_type text, vals anyarray) RETURNS text[]
    LANGUAGE sql AS $$ SELECT pgch_decode(pgch_encode_rows($1, $2)) $$;
