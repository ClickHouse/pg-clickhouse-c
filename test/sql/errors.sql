-- Rejections. Every message carries the PGCH_MSG_PREFIX the Makefile sets.
SET lc_messages = 'C';

-- NULL into a column ClickHouse declared NOT NULL.
SELECT pgch_roundtrip('Int32', NULL::int4);
SELECT pgch_roundtrip('Array(Int32)', NULL::int4[]);
SELECT pgch_roundtrip('LowCardinality(String)', NULL::text);

-- PG type the destination column cannot hold. text into Int32 pins the gate
-- on the IO cast: find_coercion_pathway offers one for a string source too,
-- and taking it would turn this into an input-syntax error at row 40000.
SELECT pgch_roundtrip('Int32', 'x'::text);
SELECT pgch_roundtrip('Array(Int32)', 1::int4);
SELECT pgch_roundtrip('Array(Int32)', ARRAY['x']::text[]);
SELECT pgch_roundtrip('IPv4', '::1'::inet);

-- No cast either way, at the column and at the element.
SELECT pgch_roundtrip('Array(Int128)', ARRAY[1]::int4[]);
SELECT pgch_roundtrip('Int32', '(1,2)'::point);

-- Values the destination cannot represent.
SELECT pgch_roundtrip('Decimal(9,2)', 'NaN'::numeric);
SELECT pgch_roundtrip('Enum8('' a'' = 1)', 'z'::text);

-- Types the encoder has no buffer shape for.
SELECT pgch_encode('Tuple(Int32)', ROW(1)::record);
SELECT pgch_encode('LowCardinality(Int32)', 1::int4);

-- Tuple into a target that is not composite, and into one of the wrong width.
CREATE TYPE twofields AS (a int, b text);
SELECT pgch_decode_as(pgch_block('Tuple(Int32)', 1, '\x2a000000'::bytea), NULL::int);
-- Width and per-field type come from convert_tuples_by_position, whose DETAIL
-- wording moves between PG versions.
\set VERBOSITY terse
SELECT pgch_decode_as(pgch_block('Tuple(Int32)', 1, '\x2a000000'::bytea),
                      NULL::twofields);
SELECT pgch_decode_as(pgch_block('Tuple(Int32, Int32)', 1, '\x2a0000002a000000'::bytea),
                      NULL::twofields);
\set VERBOSITY default

-- Types with no PG mapping.
SELECT pgch_pgtype('Int128');
SELECT pgch_pgtype('Map(String, Int32)');
SELECT pgch_pgtype('Nonsense');

-- Truncated and drifting streams.
SELECT pgch_decode('\x0103'::bytea);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1]::int4[]) ||
                   pgch_encode_rows('String', ARRAY['a']::text[]));

-- A chunk source drained mid-block is a truncation, not a clean end.
SELECT pgch_decode_chunks(substring(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[])
                                    FROM 1 FOR 12), 4);
