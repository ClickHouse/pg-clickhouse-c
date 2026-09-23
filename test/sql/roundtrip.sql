-- Round-trip PostgreSQL Datums through Native in memory
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = 'postgres';

-- Verify ClickHouse to PostgreSQL type mapping
SELECT t, pgch_pgtype(t) FROM unnest(ARRAY[
    'Int8', 'Int16', 'Int32', 'Int64',
    'UInt8', 'UInt16', 'UInt32', 'UInt64',
    'Int128', 'Int256', 'UInt128', 'UInt256',
    'Bool', 'Float32', 'Float64', 'BFloat16',
    'Decimal(9,2)', 'Decimal(38,10)',
    'String', 'FixedString(5)', 'Enum8(''a'' = 1)',
    'Date', 'Date32', 'DateTime', 'DateTime64(3)', 'IntervalDay',
    'UUID', 'IPv4', 'IPv6', 'JSON',
    'Nullable(String)', 'LowCardinality(String)',
    'LowCardinality(Nullable(String))',
    'Array(Int32)', 'Array(Array(String))', 'Tuple(Int32, String)'
]) AS t;

-- Verify integer bounds and width mappings
SELECT pgch_roundtrip('Int8', 127::int2),
       pgch_roundtrip('Int8', (-128)::int2),
       pgch_roundtrip('Int16', 32767::int2),
       pgch_roundtrip('Int32', (-2147483648)::int4),
       pgch_roundtrip('Int64', 9223372036854775807::int8);

SELECT pgch_roundtrip('UInt8', 255::int2),
       pgch_roundtrip('UInt16', 65535::int4),
       pgch_roundtrip('UInt32', 4294967295::int8),
       pgch_roundtrip('UInt64', 9223372036854775807::int8);

-- Test wide integer extremes represented as numeric
SELECT pgch_roundtrip('UInt64', 18446744073709551615::numeric),
       pgch_roundtrip('UInt128',
           340282366920938463463374607431768211455::numeric),
       pgch_roundtrip('UInt256',
           115792089237316195423570985008687907853269984665640564039457584007913129639935::numeric);

SELECT pgch_roundtrip('Int128', 170141183460469231731687303715884105727::numeric),
       pgch_roundtrip('Int128', (-170141183460469231731687303715884105728)::numeric),
       pgch_roundtrip('Int256',
           57896044618658097711785492504343953926634992332820282019728792003956564819967::numeric),
       pgch_roundtrip('Int256',
           (-57896044618658097711785492504343953926634992332820282019728792003956564819968)::numeric);

-- Mapped numeric precision spans the full range of every wide integer
SELECT pgch_decode_as(pgch_encode('UInt64', 18446744073709551615::numeric),
    NULL::numeric(20,0));
SELECT pgch_decode_as(pgch_encode('Int128',
    (-170141183460469231731687303715884105728)::numeric), NULL::numeric(39,0));
SELECT pgch_decode_as(pgch_encode('UInt128',
    340282366920938463463374607431768211455::numeric), NULL::numeric(39,0));
SELECT pgch_decode_as(pgch_encode('Int256',
    (-57896044618658097711785492504343953926634992332820282019728792003956564819968)::numeric),
    NULL::numeric(77,0));
SELECT pgch_decode_as(pgch_encode('UInt256',
    115792089237316195423570985008687907853269984665640564039457584007913129639935::numeric),
    NULL::numeric(78,0));

-- PostgreSQL unsigned 64 bit types read back through their input function
SELECT pgch_roundtrip_as('UInt64', '18446744073709551615'::xid8),
       pgch_roundtrip_as('UInt64', '0'::xid8),
       pgch_roundtrip_as('Array(UInt64)',
           ARRAY['18446744073709551615', '0']::xid8[]);

-- Unsigned targets wrap a negative value, as their input function does
SELECT pgch_decode_as(pgch_encode('Int128', (-1)::numeric), NULL::xid8);

-- Unsigned columns wrap a negative value, as ClickHouse toUInt64 does
SELECT pgch_roundtrip('UInt64', (-1)::numeric),
       pgch_roundtrip('UInt64', (-1)::int8);

-- Narrower PostgreSQL integers reach a wide column through a cast, elements too
SELECT pgch_roundtrip('Int256', 7::int8),
       pgch_roundtrip('Array(Int128)', ARRAY[1, -2]::int4[]);

-- Fractional digits fall away, as ClickHouse truncates toward zero
SELECT pgch_roundtrip('UInt64', 1.75::numeric),
       pgch_roundtrip('Int128', (-1.75)::numeric);

SELECT pgch_roundtrip('Bool', true), pgch_roundtrip('Bool', false);

SELECT pgch_roundtrip('Float32', 1.5::float4),
       pgch_roundtrip('Float64', (-2.25)::float8);

-- BFloat16 holds the leading 16 bits of a Float32, dropping the rest
SELECT pgch_roundtrip('BFloat16', 1.5::float4),
       pgch_roundtrip('BFloat16', (-2.25)::float4),
       pgch_roundtrip('BFloat16', 1.1::float4),
       pgch_encode('BFloat16', 1.5::float4);

-- Round-trip signed values across all Decimal widths
SELECT pgch_roundtrip('Decimal(9,2)', 123.45::numeric),
       pgch_roundtrip('Decimal(9,2)', (-123.45)::numeric),
       pgch_roundtrip('Decimal(18,6)', 1.000001::numeric),
       pgch_roundtrip('Decimal(38,10)', 12345678901234567890.0123456789::numeric),
       pgch_roundtrip('Decimal(76,20)', (-0.00000000000000000001)::numeric);

SELECT pgch_roundtrip('String', 'hello'::text),
       pgch_roundtrip('String', ''::text),
       pgch_roundtrip('String', E'tab\there'::text),
       pgch_roundtrip('FixedString(5)', 'abcde'::text);

-- Verify binary String data and FixedString NUL padding on wire
SELECT pgch_encode('String', '\x00ff'::bytea),
       pgch_encode('FixedString(5)', 'abc'::text);

-- Read binary String data back, bytea keeping every FixedString byte
SELECT pgch_roundtrip_as('String', '\x00ff'::bytea),
       pgch_roundtrip_as('FixedString(5)', '\x0001ff'::bytea),
       pgch_roundtrip_as('FixedString(4)', '\x01020300'::bytea);

-- Drop FixedString padding for text, which only declared type explains
SELECT pgch_decode_typed(pgch_block('FixedString(4)', 1, '\x66730000'::bytea),
                         NULL::text) AS padded,
       pgch_decode_typed(pgch_block('FixedString(4)', 1, '\x66730000'::bytea),
                         NULL::bytea) AS binary;

SELECT pgch_roundtrip('Enum8(''red'' = 1, ''green'' = 2)', 'green'::text),
       pgch_roundtrip('Enum16(''a'' = -300, ''b'' = 300)', 'a'::text);

-- Round-trip dates and times
SELECT pgch_roundtrip('Date', '2024-01-15'::date),
       pgch_roundtrip('Date32', '2024-01-15'::date),
       pgch_roundtrip('Date32', '1950-03-04'::date);

SELECT pgch_roundtrip('DateTime', '2024-01-15 12:34:56+00'::timestamptz),
       pgch_roundtrip('DateTime64(3)', '2024-01-15 12:34:56.123+00'::timestamptz),
       pgch_roundtrip('DateTime64(6)', '2024-01-15 12:34:56.123456+00'::timestamptz),
       pgch_roundtrip('DateTime64(9)', '2024-01-15 12:34:56.123456+00'::timestamptz);

SELECT pgch_roundtrip('Time', '12:34:56'::time),
       pgch_roundtrip('Time64(3)', '01:00:00'::time),
       pgch_roundtrip('Time64(6)', '12:34:56.123456'::time),
       pgch_roundtrip('Time64(9)', '23:59:59.999999'::time);

-- Each Interval unit fills one PostgreSQL interval field, decoding a count of 1
SELECT t, v AS one_unit, pgch_roundtrip_as(t, v) AS back
  FROM unnest(ARRAY[
    'IntervalNanosecond', 'IntervalMicrosecond', 'IntervalMillisecond',
    'IntervalSecond', 'IntervalMinute', 'IntervalHour',
    'IntervalDay', 'IntervalWeek',
    'IntervalMonth', 'IntervalQuarter', 'IntervalYear'
]) AS t,
  LATERAL (SELECT (pgch_decode(pgch_block(t, 1, '\x0100000000000000')))[1]::interval)
    AS s(v);

-- A day counts 24 hours, as PostgreSQL epoch extraction does
SELECT pgch_roundtrip('IntervalHour', '1 day 2 hours'::interval),
       pgch_roundtrip('IntervalSecond', '-00:01:30'::interval),
       pgch_roundtrip('IntervalNanosecond', '00:00:00.000001'::interval),
       pgch_encode('IntervalWeek', '14 days'::interval);

-- Interval columns take and return integers as raw unit counts, keeping nanoseconds
SELECT pgch_encode('IntervalNanosecond', 21::bigint),
       pgch_encode('IntervalQuarter', -2::int4);
SELECT pgch_decode_as(pgch_encode('IntervalNanosecond', 21::bigint), NULL::int8),
       pgch_decode_as(pgch_encode('IntervalQuarter', -2::int4), NULL::int2),
       pgch_decode_as(pgch_encode_rows('Nullable(IntervalSecond)',
                                       ARRAY[90, NULL]::int8[]), NULL::int4),
       pgch_decode_as(pgch_encode('Array(IntervalNanosecond)', ARRAY[21, 22]::int8[]),
                      NULL::int8[]),
       pgch_decode_as(pgch_encode('IntervalMinute', 3::int8), NULL::text);
SELECT pgch_roundtrip('Array(IntervalNanosecond)', ARRAY[21000, 22000]::int8[]),
       pgch_decode(pgch_block('Tuple(IntervalDay, Array(IntervalHour))', 1,
                              '\x0200000000000000'::bytea ||
                              '\x0100000000000000'::bytea ||
                              '\x0300000000000000'::bytea));

-- Interval columns take arrays and nulls, and String columns take an interval
SELECT pgch_roundtrip('Array(IntervalMonth)', ARRAY['1 mon', '2 mons']::interval[]),
       pgch_roundtrip('Nullable(IntervalSecond)', NULL::interval) IS NULL AS is_null,
       pgch_roundtrip('String', '3 days'::interval);

-- DateTime read as time keeps the UTC time of day, off the session zone
SET TimeZone = 'America/Los_Angeles';
SELECT pgch_decode_as(pgch_block('DateTime', 1, '\xde8c0000'), NULL::time),
       pgch_decode_typed(pgch_block('DateTime', 1, '\xde8c0000'), NULL::time),
       pgch_decode_typed(pgch_block('DateTime64(3)', 1, '\xab43260200000000'),
                         NULL::time);
SET TimeZone = 'UTC';

SELECT pgch_roundtrip('UUID', '11111111-2222-3333-4444-555555555555'::uuid),
       pgch_roundtrip('IPv4', '192.168.1.1'::inet),
       pgch_roundtrip('IPv6', '2001:db8::1'::inet);

SELECT pgch_roundtrip('JSON', '{"a": [1, 2], "b": null}'::jsonb);

-- Document text reaches json, text and bytea unparsed, jsonb normalizing it
SELECT pgch_decode_as(j, NULL::json) AS json, pgch_decode_as(j, NULL::text) AS text,
       pgch_decode_as(j, NULL::jsonb) AS jsonb, pgch_decode_as(j, NULL::bytea) AS bytea
  FROM (SELECT pgch_encode('JSON', '{"b": 1, "a": 2}'::json)) AS t(j);
SELECT pgch_decode_typed(pgch_encode('Array(JSON)', ARRAY['{"b": 1}']::jsonb[]),
                         NULL::bytea[]);

-- Round-trip nullable values and LowCardinality nulls
SELECT pgch_roundtrip('Nullable(Int32)', NULL::int4) IS NULL AS null_int,
       pgch_roundtrip('Nullable(String)', NULL::text) IS NULL AS null_text,
       pgch_roundtrip('Nullable(Int32)', 7::int4);

SELECT pgch_roundtrip_rows('Nullable(Int32)', ARRAY[1, NULL, 3]::int4[]),
       pgch_roundtrip_rows('LowCardinality(String)', ARRAY['a', 'b', 'a']::text[]),
       pgch_roundtrip_rows('LowCardinality(Nullable(String))',
                           ARRAY['a', NULL, 'a', 'b']::text[]);

-- Round-trip arrays and nested arrays
SELECT pgch_roundtrip('Array(Int32)', ARRAY[1, 2, 3]::int4[]),
       pgch_roundtrip('Array(Int32)', ARRAY[]::int4[]),
       pgch_roundtrip('Array(String)', ARRAY['a', 'b c']::text[]),
       pgch_roundtrip('Array(Nullable(Int32))', ARRAY[1, NULL, 3]::int4[]);

SELECT pgch_roundtrip('Array(Array(Int32))', ARRAY[[1, 2], [3, 4]]::int4[]),
       pgch_roundtrip('Array(Array(Array(Int32)))',
                      ARRAY[[[1], [2]], [[3], [4]]]::int4[]);

SELECT pgch_roundtrip('Array(LowCardinality(String))', ARRAY['a', 'b', 'a']::text[]),
       pgch_roundtrip('Array(Date)', ARRAY['2024-01-15', '1999-12-31']::date[]),
       pgch_roundtrip('Array(Decimal(9,2))', ARRAY[1.25, -3.5]::numeric[]);

-- Round-trip geometric types through the geo declarations they map to
CREATE FUNCTION geo_rt(lit text, typ text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE res text;
BEGIN
    EXECUTE format('SELECT pgch_roundtrip_as(pgch_chtype(%L, true), %L::%s)',
                   typ, lit, typ) INTO res;
    RETURN res;
END $$;

SELECT typ, pgch_chtype(typ, true) AS ch, geo_rt(lit, typ) AS value
FROM (VALUES
    ('point',   '(1,2)'),
    ('point',   '(1,nan)'),
    ('lseg',    '((1,1),(2,2))'),
    ('lseg',    '((1,1),(1,1))'),
    ('path',    '[(1,1),(2,2),(3,1)]'),
    ('path',    '((1,1),(2,2),(3,1))'),
    ('path',    '((1,nan))'),
    -- An open path whose ends meet is indistinguishable from a closed one
    ('path',    '[(1,1),(2,2),(1,1)]'),
    ('polygon', '((1,1),(2,2),(3,1))'),
    ('box',     '((1,1),(3,3))'),
    ('box',     '((nan,1),(3,3))'),
    ('circle',  '<(1,2),3>'),
    ('line',    '{1,-1,0}'),
    ('line',    '{nan,1,nan}')
) g(typ, lit);

-- Round-trip arrays of geometric types
SELECT pgch_roundtrip_as('Array(Ring)',
                         ARRAY['((0,0),(1,1),(2,0))',
                               '((5,5),(6,6),(7,5))']::polygon[]) AS rings,
       pgch_roundtrip_as('Array(LineString)',
                         ARRAY['[(0,0),(1,1)]', '((5,5),(6,6))']::path[]) AS lines,
       pgch_roundtrip_as('Array(Nullable(Tuple(high Point, low Point)))',
                         ARRAY['((1,1),(3,3))', NULL]::box[]) AS boxes;

SELECT pgch_decode_typed(
    pgch_encode('Array(Polygon)',
                ARRAY[ARRAY['((0,0),(1,1),(2,0))']]::polygon[]),
    NULL::polygon[]
);

-- Round-trip multi-geometries, which PostgreSQL spells as arrays
SELECT pgch_roundtrip_as('Polygon',
                         ARRAY['((0,0),(1,1),(2,0))', '((5,5),(6,6),(7,5))']::polygon[]),
       pgch_roundtrip_as('MultiLineString',
                         ARRAY['[(0,0),(1,1)]', '((5,5),(6,6))']::path[]),
       pgch_roundtrip_as('MultiPolygon',
                         ARRAY[ARRAY['((0,0),(1,1),(2,0))'],
                               ARRAY['((5,5),(6,6),(7,5))']]::polygon[]);

-- Read geo columns into the other geometric types PostgreSQL has
SELECT pgch_decode_typed(pgch_encode('LineString', '((1,1),(2,2))'::lseg),
                         NULL::lseg) AS line_as_lseg,
       pgch_decode_typed(pgch_encode('Ring', '((0,0),(1,1),(2,0))'::polygon),
                         NULL::box) AS ring_as_box,
       pgch_decode_typed(pgch_encode('Ring', '((0,0),(1,1),(2,0))'::polygon),
                         NULL::path) AS ring_as_path;

-- Reject a line that is not two points as lseg
SELECT pgch_decode_typed(pgch_encode('LineString', '[(1,1),(2,2),(3,3)]'::path),
                         NULL::lseg);

-- Use PostgreSQL casts for types without direct encoder support
SET lc_monetary = 'C';
CREATE DOMAIN dint AS int4;
SELECT pgch_roundtrip('UInt32', 12345::oid),
       pgch_roundtrip('Int32', 7::dint),
       pgch_roundtrip('UInt64', '42'::xid8),
       pgch_roundtrip('Decimal(9,2)', '3.00'::money),
       pgch_roundtrip('Int64', '42'::jsonb);

SELECT pgch_roundtrip('String', 'vc'::varchar),
       pgch_roundtrip('String', 'bp'::bpchar),
       pgch_roundtrip('String', 'nm'::name),
       pgch_roundtrip('String', 'c'::"char"),
       pgch_roundtrip('FixedString(2)', 'fs'::varchar),
       pgch_roundtrip('Enum8(''1 day'' = 1)', '1 day'::interval);

-- Apply PostgreSQL casts to array elements
SELECT pgch_roundtrip('Array(String)', ARRAY['3.00', '-4.50']::money[]),
       pgch_roundtrip('Array(Nullable(String))', ARRAY['1 day', NULL]::interval[]),
       pgch_roundtrip('Array(Array(String))', ARRAY[['1 day'], ['2 hours']]::interval[]);

-- Decode multiple rows and blocks as one stream
SELECT pgch_roundtrip_rows('Int32', ARRAY[1, 2, 3]::int4[]);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]) ||
                   pgch_encode_rows('Int32', ARRAY[3]::int4[]));

-- Decode blocks delivered across small chunks
SELECT pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]) ||
                          pgch_encode_rows('Int32', ARRAY[3]::int4[]), 1),
       pgch_decode_chunks(pgch_encode_rows('String', ARRAY['a', 'bb']::text[]), 3),
       pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[]::int4[]), 4);

-- Preserve schema in empty blocks
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[]::int4[]));

-- Pin Native output for one Int32 column and three rows
SELECT pgch_encode_rows('Int32', ARRAY[1, 2, 3]::int4[]);

-- Skip invalid value without losing complete rows
SELECT pgch_decode(pgch_encode_valid_rows(
    'Nullable(Enum8(''ok'' = 1))', ARRAY['ok', 'bad', 'ok']::text[]));

-- Decode Tuple into record
SELECT pgch_decode(
    '\x0102'::bytea ||
    '\x01' || convert_to('c', 'UTF8') ||
    '\x14' || convert_to('Tuple(Int32, String)', 'UTF8') ||
    '\x2a000000ffffffff'::bytea ||
    '\x02' || convert_to('hi', 'UTF8') ||
    '\x03' || convert_to('bye', 'UTF8')
);

-- Decode Tuple into composite by field position through both setup APIs
CREATE TYPE tupformat AS (a int, b text, c float4);
SELECT pgch_decode_as(b, NULL::tupformat) AS from_value,
       pgch_decode_typed(b, NULL::tupformat) AS from_type
FROM pgch_block('Tuple(Int32, String, Float32)', 2,
                '\x2a000000ffffffff'::bytea ||
                '\x02' || convert_to('hi', 'UTF8') || '\x00'::bytea ||
                '\x0000c03f00002040'::bytea) AS b;

-- Decode nested Tuple when first row is present or NULL
CREATE TYPE nested_inner AS (a int);
CREATE TYPE nested_outer AS (t nested_inner, b text);
SELECT pgch_decode_as(b, NULL::nested_outer) AS from_value,
       pgch_decode_typed(b, NULL::nested_outer) AS from_type
FROM pgch_block('Tuple(Tuple(Int32), String)', 1,
                '\x07000000'::bytea || '\x03' || convert_to('end', 'UTF8')) AS b;
SELECT pgch_decode_as(b, NULL::nested_outer) AS from_value,
       pgch_decode_typed(b, NULL::nested_outer) AS from_type
FROM pgch_block('Tuple(Nullable(Tuple(Int32)), String)', 2,
                '\x0100'::bytea || '\x0000000009000000'::bytea ||
                '\x00'::bytea || '\x02' || convert_to('ok', 'UTF8')) AS b;

-- Spread Tuple fields over an array where no composite type names the record
SELECT pgch_decode_as(b, NULL::text[]) AS from_value,
       pgch_decode_typed(b, NULL::text[]) AS from_type
FROM pgch_block('Tuple(Int32, String)', 1,
                '\x2a000000'::bytea || '\x02' || convert_to('hi', 'UTF8')) AS b;

-- Widen unlike Tuple fields into one element type, NULL fields included
SELECT pgch_decode_as(b, NULL::bigint[]) AS from_value,
       pgch_decode_typed(b, NULL::bigint[]) AS from_type
FROM pgch_block('Tuple(Nullable(Int16), Int32, String)', 2,
                '\x0100'::bytea || '\x00002a00'::bytea ||
                '\x3905000002000000'::bytea ||
                '\x01' || convert_to('7', 'UTF8') ||
                '\x01' || convert_to('8', 'UTF8')) AS b;

-- Fields of a Tuple inside an Array fill the inner dimension
SELECT pgch_decode_as(b, NULL::numeric[])
FROM pgch_block('Array(Tuple(Int16, Decimal(10, 4)))', 1,
                '\x0200000000000000'::bytea ||
                '\x2a00'::bytea || '\x0100'::bytea ||
                '\x3905000000000000'::bytea || '\x0200000000000000'::bytea) AS b;

-- Map writes and reads as Array(Tuple(K, V)), so both spellings agree
CREATE TYPE pairformat AS (k text, v bigint);
SELECT pgch_decode(pgch_encode_pairs('Map(String, Int64)',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[])) AS map,
       pgch_decode(pgch_encode_pairs('Array(Tuple(String, Int64))',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[])) AS array_tuple;
SELECT pgch_decode_as(pgch_encode_pairs('Map(String, Nullable(Int64))',
                                        ARRAY['a', 'b'], ARRAY[1, NULL]::bigint[]),
                      NULL::pairformat[]);
SELECT pgch_decode(pgch_encode_pairs('Map(String, Int64)',
                                     ARRAY[]::text[], ARRAY[]::bigint[]));
SELECT pgch_pgtype('Map(String, Int64)');
-- An Array field is an unbuilt intermediate, not an array item, so its Tuple
-- stays a record
SELECT pgch_decode_as(b, NULL::text[]) AS from_value,
       pgch_decode_typed(b, NULL::text[]) AS from_type
FROM pgch_block('Map(String, Array(Int32))', 1,
                '\x0100000000000000'::bytea ||
                '\x01' || convert_to('a', 'UTF8') ||
                '\x0100000000000000'::bytea || '\x07000000'::bytea) AS b;

-- Encode Map from key-value arrays, converting each item to its field type
SELECT pgch_decode_as(pgch_encode('Map(String, Int64)',
                                  ARRAY[['a', '1'], ['b', '2']]::text[]),
                      NULL::pairformat[]) AS pairs,
       pgch_decode_as(pgch_encode('Map(String, Nullable(Int64))',
                                  ARRAY[['a', NULL]]::text[]),
                      NULL::pairformat[]) AS null_value,
       pgch_decode_as(pgch_encode('Map(String, Int64)', ARRAY[]::text[]),
                      NULL::pairformat[]) AS empty;
-- Encode Tuple from an array of fields
SELECT pgch_decode_as(pgch_encode('Tuple(String, Int64)', ARRAY['a', '1']::text[]),
                      NULL::pairformat) AS tuple;
-- Nest tuples through the cursor, Nullable wrapping the inner one
SELECT pgch_decode(pgch_encode_pairs('Map(String, Tuple(Int64))',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[],
                                     2, true)) AS nested,
       pgch_decode(pgch_encode_pairs('Array(Tuple(String, Nullable(Tuple(Int64))))',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[],
                                     2, true)) AS nullable_nested;

-- Expand untyped Tuple fields into a text array dimension
SELECT (pgch_decode(pgch_encode_pairs('Nested(k String, v Int64)',
                                      ARRAY['a', 'b'], ARRAY[1, 2]::bigint[])))[1] AS nested,
       (pgch_decode(pgch_encode_pairs('Array(Tuple(k String, v Int64))',
                                      ARRAY['a', 'b'], ARRAY[1, 2]::bigint[])))[1] AS array_tuple;
-- Convert both declarations into pairformat[] before rendering
SELECT (pgch_decode_as(pgch_encode_pairs('Nested(k String, v Int64)',
                                         ARRAY['a', 'b'], ARRAY[1, 2]::bigint[]),
                       NULL::pairformat[]))[1] AS nested,
       (pgch_decode_as(pgch_encode_pairs('Array(Tuple(k String, v Int64))',
                                         ARRAY['a', 'b'], ARRAY[1, 2]::bigint[]),
                       NULL::pairformat[]))[1] AS array_tuple;
-- Preserve NULL fields in composite array elements
SELECT (pgch_decode_as(pgch_encode_pairs('Nested(k String, v Nullable(Int64))',
                                         ARRAY['a', 'b'], ARRAY[1, NULL]::bigint[]),
                       NULL::pairformat[]))[1] AS null_value;
SELECT pgch_pgtype('Nested(k String, v Int64)');

-- SimpleAggregateFunction stores values as its argument type
SELECT pgch_roundtrip('SimpleAggregateFunction(sum, Int64)', 7::bigint) AS sum,
       pgch_roundtrip('SimpleAggregateFunction(anyLast, LowCardinality(String))',
                      'a'::text) AS any_last,
       pgch_roundtrip_rows('SimpleAggregateFunction(anyLast, Nullable(Int32))',
                           ARRAY[1, NULL]::int4[]) AS nullable,
       pgch_roundtrip('SimpleAggregateFunction(groupArrayArray, Array(Int32))',
                      ARRAY[1, 2]::int4[]) AS group_array;

-- Convert values whose PostgreSQL type the column kind does not take directly
SELECT pgch_roundtrip('Int32', true) AS bool,
       pgch_roundtrip('Int32', 1.5::float4) AS float4,
       pgch_roundtrip('Int32', 2.5::float8) AS float8,
       pgch_roundtrip('String', 1.5::numeric) AS numeric;

-- Read timestamp without time zone through the session timezone
SELECT pgch_roundtrip('DateTime', '2020-01-02 03:04:05'::timestamp) AS datetime,
       pgch_roundtrip('DateTime64(3)', '2020-01-02 03:04:05.5'::timestamp) AS scaled,
       pgch_roundtrip('DateTime64(3)',
                      '1900-01-01 00:00:00.5'::timestamptz) AS before_epoch;

-- Take the midpoint of a two-point list as a Point
SELECT pgch_roundtrip('Point', '[(0,0),(1,1)]'::lseg) AS lseg,
       pgch_roundtrip('Point', '((0,0),(1,1))'::box) AS box;

-- Nothing and Void carry one byte per row, every value NULL
SELECT pgch_decode(pgch_block('Nothing', 1, '\x00'::bytea)) AS nothing,
       pgch_decode(pgch_block('Void', 1, '\x00'::bytea)) AS void;

-- Compressed values arrive as a copy the encoder frees after writing
CREATE TABLE toasted (t text, j json);
INSERT INTO toasted VALUES (repeat('a', 4000), ('"' || repeat('b', 4000) || '"')::json);
SELECT length((pgch_decode(pgch_encode('String', t)))[1]) AS text,
       length((pgch_decode(pgch_encode('String', j)))[1]) AS json FROM toasted;

-- Nest multi-geometries a level deeper inside an array
SELECT pgch_roundtrip_as('Array(MultiLineString)',
                         ARRAY[ARRAY['[(0,0),(1,1)]']]::path[]) AS lines,
       pgch_roundtrip_as('Array(MultiPolygon)',
                         ARRAY[ARRAY[ARRAY['((0,0),(1,1),(2,0))']]]::polygon[]) AS polys;

-- Bound every ring point when the target is a box, take two points as an lseg
SELECT pgch_decode_as(pgch_encode('Ring', '((2,2),(0,0),(1,3))'::polygon),
                      NULL::box) AS bbox,
       pgch_decode_as(pgch_encode('Ring', '((2,2),(0,0))'::polygon),
                      NULL::lseg) AS lseg;

-- Convert UInt8 into boolean, Nothing into any target
SELECT pgch_decode_as(pgch_encode('UInt8', 1::int2), NULL::bool) AS bool,
       pgch_decode_as(pgch_block('Nothing', 1, '\x00'::bytea), NULL::int4) AS nothing;

-- Take String into a target carrying no type modifier
SELECT pgch_decode_as(pgch_encode('String', 'x'::text), NULL::varchar) AS varchar;

-- Read an empty array row
SELECT pgch_decode_as(pgch_encode('Array(Int32)', ARRAY[]::int4[]),
                      NULL::int4[]) AS empty;

-- Read an enum value the dictionary does not hold
SELECT pgch_decode(pgch_block('Enum8(''a'' = 1)', 1, '\x02'::bytea)) AS unknown;

-- Keep rows written before a failing value, whatever the column layout
SELECT pgch_decode(pgch_encode_valid_rows('String',
                                          ARRAY['a', NULL, 'b']::text[])) AS string,
       pgch_decode(pgch_encode_valid_rows('LowCardinality(String)',
                                          ARRAY['a', NULL]::text[])) AS low_cardinality,
       pgch_decode(pgch_encode_valid_rows('Map(String, Int64)',
                                          ARRAY['a']::text[])) AS map;

-- Write NULL into Nullable JSON, whose value ClickHouse validates regardless
SELECT pgch_roundtrip('Nullable(JSON)', NULL::jsonb) AS null_json;

-- Read LowCardinality keys of every width ClickHouse writes
SELECT pgch_decode(pgch_block('LowCardinality(String)', 1,
                              '\x0100000000000000'::bytea ||
                              '\x0006000000000000'::bytea ||
                              '\x0100000000000000'::bytea || '\x0161'::bytea ||
                              '\x0100000000000000'::bytea || '\x00'::bytea)) AS key1,
       pgch_decode(pgch_block('LowCardinality(String)', 1,
                              '\x0100000000000000'::bytea ||
                              '\x0106000000000000'::bytea ||
                              '\x0100000000000000'::bytea || '\x0161'::bytea ||
                              '\x0100000000000000'::bytea || '\x0000'::bytea)) AS key2,
       pgch_decode(pgch_block('LowCardinality(String)', 1,
                              '\x0100000000000000'::bytea ||
                              '\x0306000000000000'::bytea ||
                              '\x0100000000000000'::bytea || '\x0161'::bytea ||
                              '\x0100000000000000'::bytea ||
                              '\x0000000000000000'::bytea)) AS key8;

-- Write NULL into every nullable column layout
SELECT pgch_roundtrip('Nullable(IPv6)', NULL::inet) AS ipv6,
       pgch_roundtrip('Nullable(FixedString(4))', NULL::text) AS fixed_string,
       pgch_roundtrip('Nullable(Enum8(''a'' = 1))', NULL::text) AS enum,
       pgch_roundtrip('Nullable(Array(Int32))', NULL::int4[]) AS array;

-- Relabel a binary-coercible target, take Nothing into any target
SELECT pgch_decode_typed(pgch_encode('Int32', 1::int4), NULL::oid) AS oid,
       pgch_decode_typed(pgch_block('Nothing', 1, '\x00'::bytea),
                         NULL::int4) AS nothing;

-- Convert an empty array row, whose element type still governs the result
SELECT pgch_decode_as(pgch_encode('Array(Int32)', ARRAY[]::int4[]),
                      NULL::int8[]) AS empty;

-- Spread a nested Tuple's axes over the target's coordinates
SELECT pgch_decode_as(pgch_block('Tuple(Tuple(Float64, Float64), Float64)', 1,
                                 '\x000000000000f03f'::bytea ||
                                 '\x0000000000000040'::bytea ||
                                 '\x0000000000000840'::bytea), NULL::line) AS line;

-- Read a Tuple into a domain over a composite type
CREATE DOMAIN pairdom AS pairformat;
SELECT pgch_decode_typed(pgch_encode_pairs('Map(String, Int64)', ARRAY['a'],
                                           ARRAY[1]::bigint[]),
                         NULL::pairdom[]) AS pairs;

-- Convert array rows that hold no values, at the outer and the inner dimension
SELECT pgch_decode_as(pgch_block('Array(Array(Int32))', 1,
                                 '\x0000000000000000'::bytea),
                      NULL::int8[]) AS empty_outer,
       pgch_decode_as(pgch_block('Array(Array(Int32))', 1,
                                 '\x0100000000000000'::bytea ||
                                 '\x0000000000000000'::bytea),
                      NULL::int8[]) AS empty_inner;

-- Read a Tuple into a domain over a composite type
SELECT pgch_decode_typed(pgch_encode('Tuple(String, Int64)',
                                     ARRAY['a', '1']::text[]),
                         NULL::pairdom) AS pair;

-- PostgreSQL 19 added oid8, which takes UInt64 the way xid8 does
CREATE FUNCTION decode_as_type(data bytea, target text) RETURNS text
    LANGUAGE plpgsql AS $$
DECLARE
    out text;
BEGIN
    EXECUTE format('SELECT (pgch_decode_as($1, NULL::%s))[1]', target)
        INTO out USING data;
    RETURN out;
END $$;
SELECT decode_as_type(pgch_encode('UInt64', 42::numeric),
                      coalesce(to_regtype('oid8'), 'xid8'::regtype)::text) AS unsigned64;
DROP FUNCTION decode_as_type;
