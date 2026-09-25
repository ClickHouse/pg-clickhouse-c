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

-- Reject missing scalar casts
SELECT pgch_roundtrip('Int32', '(1,2)'::point);

-- Reject values outside destination domain
SELECT pgch_roundtrip('Decimal(9,2)', 'NaN'::numeric);
SELECT pgch_roundtrip('Enum8('' a'' = 1)', 'z'::text);
SELECT pgch_roundtrip('Decimal(9,2)', 1000000000::numeric);
SELECT pgch_roundtrip('Decimal(18,0)', 99999999999999999999::numeric);

-- Reject composite values whose field count differs from Tuple
SELECT pgch_encode('Tuple(Int32)', ROW(1, 2));
SELECT pgch_encode('Tuple(Int32, Int32)', ROW(1));
-- Reject unsupported encoder types
SELECT pgch_encode('LowCardinality(Array(Int32))', ARRAY[1]::int4[]);
SELECT pgch_encode('Array(Dynamic)', ARRAY[1]::int4[]);

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
SELECT pgch_encode('Map(String, Int64)', ARRAY[ROW('a')]);
SELECT pgch_encode('Map(String, Int64)', ARRAY[ROW('a', 1, 2)]);
SELECT pgch_encode('Map(String)', ARRAY[ROW('a', 1)]);
SELECT pgch_encode('Nested(k String, v Int64)', ARRAY[ROW('a')]);
SELECT pgch_encode('Nested', ARRAY['a']::text[]);

-- Reject invalid Map arrays
SELECT pgch_encode('Map(String, Int64)', ARRAY['a', '1']::text[]);
SELECT pgch_encode('Map(String, Int64)', ARRAY[['a', '1', 'x']]::text[]);
SELECT pgch_encode('Map(String, Int64)', ARRAY[['a', 'x']]::text[]);
SELECT pgch_encode('Map(String, Int64)', NULL::text[]);

-- Reject types without PostgreSQL mapping
SELECT pgch_pgtype('Dynamic');
SELECT pgch_pgtype('Nonsense');

-- Reject unsupported types before reading rows, including nested types
SELECT pgch_decode(pgch_block('Tuple()', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Map(String)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Nested', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Nested(a Dynamic)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('SimpleAggregateFunction(anyLast, Dynamic)', 0,
                              ''::bytea));
SELECT pgch_decode(pgch_block('Array(Nothing)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Dynamic)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Array(Dynamic)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('Tuple(Int32, Map(String, Dynamic))', 0, ''::bytea));
-- Identify unnamed columns by position
SELECT pgch_decode('\x01000007'::bytea || convert_to('Dynamic', 'UTF8'));

-- Accept supported LowCardinality String forms
SELECT pgch_decode(pgch_block('LowCardinality(String)', 0, ''::bytea));
SELECT pgch_decode(pgch_block('LowCardinality(Nullable(String))', 0, ''::bytea));

-- Reject UInt64 values a bigint target cannot hold
SELECT pgch_decode_as(pgch_block('UInt64', 1, '\xffffffffffffffff'::bytea),
                      NULL::int8);

-- Reject integers wider than the ClickHouse column
SELECT pgch_roundtrip('UInt64', 18446744073709551616::numeric);
SELECT pgch_roundtrip('Int128', 170141183460469231731687303715884105728::numeric);
SELECT pgch_roundtrip('UInt256',
    115792089237316195423570985008687907853269984665640564039457584007913129639936::numeric);

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
-- Interval units convert differently from one another
SELECT pgch_decode(pgch_encode_rows('IntervalSecond', ARRAY[1]::int8[]) ||
                   pgch_encode_rows('IntervalDay', ARRAY[1]::int8[]));

-- Reject chunk streams ending within a block
SELECT pgch_decode_chunks(substring(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[])
                                    FROM 1 FOR 12), 4);

-- Report status instead of message, which names the database encoding
CREATE FUNCTION decode_status(data bytea, target text) RETURNS text
    LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE format('SELECT pgch_decode_as($1, NULL::%s)', target) USING data;
    RETURN 'ok';
EXCEPTION WHEN character_not_in_repertoire THEN
    RETURN 'invalid encoding';
WHEN invalid_text_representation THEN
    RETURN 'invalid document';
END $$;

-- Reject ClickHouse bytes PostgreSQL cannot read as text, bytea taking them as they are
SELECT decode_status(s, 'text') AS text, decode_status(s, 'bytea') AS bytea
  FROM (SELECT pgch_block('String', 1, '\x0361006e'::bytea)) AS t(s);
SELECT decode_status(a, 'text[]') AS text, decode_status(a, 'bytea[]') AS bytea
  FROM (SELECT pgch_block('Array(String)', 1,
                          '\x0100000000000000'::bytea ||
                          '\x0361006e'::bytea)) AS t(a);
SELECT decode_status(j, 'jsonb') AS valid,
       decode_status(set_byte(j, octet_length(j) - 3, 0), 'jsonb') AS embedded_nul,
       decode_status(set_byte(j, octet_length(j) - 3, 0), 'bytea') AS nul_bytea
  FROM (SELECT pgch_encode('JSON', '{"a": "b"}'::jsonb)) AS t(j);

-- Parse the document only for a target that stores JSON
SELECT decode_status(d, 'bytea') AS bytea, decode_status(d, 'text') AS text,
       decode_status(d, 'jsonb') AS jsonb
  FROM (SELECT set_byte(j, octet_length(j) - 1, 120)
          FROM (SELECT pgch_encode('JSON', '{"a": "b"}'::jsonb)) AS s(j)) AS t(d);

-- Drop trailing NUL padding without declared ClickHouse type
SELECT decode_status(f, 'text') AS text, decode_status(f, 'bytea') AS bytea
  FROM (SELECT pgch_block('FixedString(4)', 1, '\x66730000'::bytea)) AS t(f);

DROP FUNCTION decode_status;

-- Reject values with no cast to the column's PostgreSQL type
SELECT pgch_roundtrip('IPv4', 1::numeric);
SELECT pgch_roundtrip('Int32', '2020-01-02'::date);
SELECT pgch_roundtrip('Int32', '01:02:03'::time);
SELECT pgch_roundtrip('Int32', '2020-01-02 03:04:05+00'::timestamptz);
SELECT pgch_roundtrip('Int32', ARRAY[1]::int4[]);
SELECT pgch_roundtrip('Int32', '11111111-2222-3333-4444-555555555555'::uuid);
-- PostgreSQL 14 catalogs an unimplemented path -> point cast
SELECT pgch_roundtrip('Int32', '((0,0),(1,1))'::path);
SELECT pgch_roundtrip('Point', '{1,2,3}'::line);

-- Reject timestamps outside the scaled DateTime64 range
SELECT pgch_roundtrip('DateTime64(9)', '9999-01-01'::timestamptz);

-- Reject targets with no conversion from the column type
SELECT pgch_decode_as(pgch_encode('Int32', 1::int4), NULL::point);

-- Reject Tuple columns without fields
SELECT pgch_encode('Tuple()', ARRAY[]::text[]);
SELECT pgch_decode(pgch_block('Array(Tuple())', 0, ''::bytea));

-- Reject blocks carrying no columns
SELECT pgch_decode('\x0000'::bytea);

-- Reject nested arrays where the column takes one dimension
SELECT pgch_encode('Array(Int32)', ARRAY[[1, 2]]::int4[]);
SELECT pgch_encode('Array(Tuple(Int32))', ARRAY[[NULL]]::int4[]);

-- Reject coordinates into a Tuple that holds anything but Float64
SELECT pgch_encode('Tuple(Int32, Int32)', '((0,0),(1,1))'::box);

-- Reject Tuples the target's coordinates cannot take
SELECT pgch_decode_as(pgch_block('Tuple(Nullable(Float64), Float64, Float64)', 1,
                                 '\x01'::bytea || '\x0000000000000000'::bytea ||
                                 '\x000000000000f03f'::bytea ||
                                 '\x0000000000000040'::bytea), NULL::line);
SELECT pgch_decode_as(pgch_block('Tuple(Float64, Float64, Float64, Float64, Float64)',
                                 1, ('\x' || repeat('00', 40))::bytea), NULL::line);
SELECT pgch_decode_as(pgch_block('Tuple(String, Float64, Float64)', 1,
                                 '\x0161'::bytea ||
                                 '\x0000000000000000'::bytea ||
                                 '\x0000000000000000'::bytea), NULL::line);

-- Reject arrays nested deeper than PostgreSQL allows
SELECT pgch_decode_as(
    pgch_block('Array(Array(Array(Array(Array(Array(Array(Int32)))))))', 1,
               ('\x' || repeat('0100000000000000', 7))::bytea ||
               '\x2a000000'::bytea), NULL::int4[]);

-- Reject blocks whose column count changes mid-stream
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1]::int4[]) ||
                   '\x0201'::bytea ||
                   '\x01' || convert_to('c', 'UTF8') ||
                   '\x05' || convert_to('Int32', 'UTF8') || '\x01000000'::bytea ||
                   '\x01' || convert_to('d', 'UTF8') ||
                   '\x05' || convert_to('Int32', 'UTF8') || '\x02000000'::bytea);

-- Reject point pairs the target's coordinate count cannot take
SELECT pgch_decode_as(pgch_encode('Tuple(Point, Point)', ARRAY['(0,0)', '(1,1)']::point[]),
                      NULL::line);

-- Reject coordinates the Tuple has too few fields to hold
SELECT pgch_encode('Tuple(Float64, Float64)', '((0,0),(1,1))'::box);
