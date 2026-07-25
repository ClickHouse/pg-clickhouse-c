-- Verify rejected values and prefixed errors
SET lc_messages = 'C';

-- Reject NULL for non-nullable columns
SELECT pgch_roundtrip('Int32', NULL::int4);
SELECT pgch_roundtrip('Array(Int32)', NULL::int4[]);
SELECT pgch_roundtrip('LowCardinality(String)', NULL::text);

-- Reject incompatible PostgreSQL source types
SELECT pgch_roundtrip('Int32', 'x'::text);
SELECT pgch_roundtrip('Array(Int32)', 1::int4);
SELECT pgch_roundtrip('Array(Int32)', ARRAY['x']::text[]);
SELECT pgch_roundtrip('IPv4', '::1'::inet);

-- Reject missing scalar and element casts
SELECT pgch_roundtrip('Array(Int128)', ARRAY[1]::int4[]);
SELECT pgch_roundtrip('Int32', '(1,2)'::point);

-- Reject values outside destination domain
SELECT pgch_roundtrip('Decimal(9,2)', 'NaN'::numeric);
SELECT pgch_roundtrip('Enum8('' a'' = 1)', 'z'::text);

-- Reject unsupported encoder types
SELECT pgch_encode('Tuple(Int32)', ROW(1)::record);
SELECT pgch_encode('LowCardinality(Int32)', 1::int4);

-- Reject Tuple targets with incompatible shape
CREATE TYPE twofields AS (a int, b text);
SELECT pgch_decode_as(pgch_block('Tuple(Int32)', 1, '\x2a000000'::bytea), NULL::int);
-- PostgreSQL changes composite conversion detail text between versions
\set VERBOSITY terse
SELECT pgch_decode_as(pgch_block('Tuple(Int32)', 1, '\x2a000000'::bytea),
                      NULL::twofields);
SELECT pgch_decode_as(pgch_block('Tuple(Int32, Int32)', 1, '\x2a0000002a000000'::bytea),
                      NULL::twofields);
\set VERBOSITY default

-- Reject types without PostgreSQL mapping
SELECT pgch_pgtype('Int128');
SELECT pgch_pgtype('Map(String, Int32)');
SELECT pgch_pgtype('Nonsense');

-- Reject unsupported types before reading rows, including nested types
SELECT pgch_decode(pgch_block('Map(String, Int32)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Tuple()', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Array(Nothing)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Int32)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Array(Int128)', 1, '\x0000000000000000'::bytea));
SELECT pgch_decode(pgch_block('Tuple(Int32, Map(String, Int32))', 0, ''::bytea));
-- Identify unnamed columns by position
SELECT pgch_decode('\x01000006'::bytea || convert_to('Int128', 'UTF8'));

-- Accept supported LowCardinality String forms
SELECT pgch_decode(pgch_block('LowCardinality(String)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Nullable(String))', 0, ''::bytea));

-- Reject UInt64 values outside bigint range
SELECT pgch_decode(pgch_block('UInt64', 1, '\xffffffffffffffff'::bytea));

-- Reject truncated streams and incompatible schema changes
SELECT pgch_decode('\x0103'::bytea);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1]::int4[]) ||
                   pgch_encode_rows('String', ARRAY['a']::text[]));

-- Reject chunk streams ending within a block
SELECT pgch_decode_chunks(substring(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[])
                                    FROM 1 FOR 12), 4);
