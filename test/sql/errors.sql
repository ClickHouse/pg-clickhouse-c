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
SELECT pgch_roundtrip('Decimal(9,2)', 1000000000::numeric);
SELECT pgch_roundtrip('Decimal(18,0)', 99999999999999999999::numeric);

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
-- Reject Tuple fields the array's element type cannot take
SELECT pgch_decode_as(pgch_block('Tuple(Int32, String)', 1,
                                 '\x2a000000'::bytea || '\x02' ||
                                 convert_to('hi', 'UTF8')),
                      NULL::bigint[]);

-- Reject Tuple field counts other than the column's arity
SELECT pgch_encode_pairs('Map(String, Int64)', ARRAY['a'], ARRAY[1]::bigint[], 1);
SELECT pgch_encode_pairs('Map(String, Int64)', ARRAY['a'], ARRAY[1]::bigint[], 3);
SELECT pgch_encode_pairs('Tuple(String, Int64)', ARRAY['a'], ARRAY[1]::bigint[]);
SELECT pgch_encode_pairs('Map(String)', ARRAY['a'], ARRAY[1]::bigint[]);

-- Reject invalid Map arrays
SELECT pgch_encode('Map(String, Int64)', ARRAY['a', '1']::text[]);
SELECT pgch_encode('Map(String, Int64)', ARRAY[['a', '1', 'x']]::text[]);
SELECT pgch_encode('Map(String, Int64)', ARRAY[['a', 'x']]::text[]);
SELECT pgch_encode('Map(String, Int64)', NULL::text[]);

-- Reject types without PostgreSQL mapping
SELECT pgch_pgtype('Int128');
SELECT pgch_pgtype('Nonsense');

-- Reject unsupported types before reading rows, including nested types
SELECT pgch_decode(pgch_block('Tuple()', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Map(String)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Array(Nothing)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Int32)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Array(Int128)', 1, '\x0000000000000000'::bytea));
SELECT pgch_decode(pgch_block('Tuple(Int32, Map(String, Int128))', 0, ''::bytea));
-- Identify unnamed columns by position
SELECT pgch_decode('\x01000006'::bytea || convert_to('Int128', 'UTF8'));

-- Accept supported LowCardinality String forms
SELECT pgch_decode(pgch_block('LowCardinality(String)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Nullable(String))', 0, ''::bytea));

-- Reject UInt64 values outside bigint range
SELECT pgch_decode(pgch_block('UInt64', 1, '\xffffffffffffffff'::bytea));

-- Reject DateTime64 outside timestamp range
SELECT pgch_decode(pgch_block('DateTime64(0)', 1, '\x0000000000000080'::bytea));

-- Reject Date32 values outside PostgreSQL date range
SELECT pgch_decode(pgch_block('Date32', 1, '\x60dad9ff'::bytea));
SELECT pgch_decode(pgch_block('Date32', 1, '\x00000080'::bytea));

-- Reject intervals the ClickHouse unit cannot hold, months having no length
SELECT pgch_roundtrip('IntervalDay', '36 hours'::interval);
SELECT pgch_roundtrip('IntervalHour', '1 mon'::interval);
SELECT pgch_roundtrip('IntervalYear', '18 mons'::interval);

-- Reject Interval counts outside the PostgreSQL interval field
SELECT pgch_decode(pgch_block('IntervalYear', 1, '\xffffffff00000000'::bytea));

-- Reject Time and Time64 values greater than one day
SELECT pgch_decode(pgch_block('Time', 1, '\xb0df3600'::bytea));
SELECT pgch_decode(pgch_block('Time64(0)', 1, '\xb0df360000000000'::bytea));

-- Reject values that violate target domain constraints
CREATE DOMAIN epos AS int4 CHECK (VALUE > 0);
SELECT pgch_decode_as(pgch_encode('Int32', -1::int4), NULL::epos);
SELECT pgch_decode_as(pgch_encode('String', '-1'::text), NULL::epos);

-- Reject array elements wider than the target type modifier
SELECT pgch_decode_as(pgch_encode('Array(String)',
                                  ARRAY['abc']::text[]), NULL::varchar(2)[]);

-- Reject values wider than a FixedString, which ClickHouse also rejects
SELECT pgch_roundtrip('FixedString(4)', 'abcde'::text);
SELECT pgch_roundtrip('Array(FixedString(2))', ARRAY['ab', 'cde']::text[]);

-- Reject nested arrays PostgreSQL cannot represent, [[1,2],[3]]
SELECT pgch_decode(pgch_block('Array(Array(Int32))', 1,
                              '\x0200000000000000'::bytea ||
                              '\x02000000000000000300000000000000'::bytea ||
                              '\x010000000200000003000000'::bytea));

-- Reject forged array offsets that would read past inner column
SELECT pgch_decode(pgch_block('Array(Int32)', 2,
                              '\x05000000000000000200000000000000'::bytea ||
                              '\x0100000002000000'::bytea));

-- Reject truncated streams and incompatible schema changes
SELECT pgch_decode('\x0103'::bytea);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1]::int4[]) ||
                   pgch_encode_rows('String', ARRAY['a']::text[]));

-- Reject chunk streams ending within a block
SELECT pgch_decode_chunks(substring(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[])
                                    FROM 1 FOR 12), 4);
