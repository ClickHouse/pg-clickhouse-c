-- Round-trip PostgreSQL Datums through Native in memory
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = 'postgres';

-- Verify ClickHouse to PostgreSQL type mapping
SELECT t, pgch_pgtype(t) FROM unnest(ARRAY[
    'Int8', 'Int16', 'Int32', 'Int64',
    'UInt8', 'UInt16', 'UInt32', 'UInt64',
    'Bool', 'Float32', 'Float64',
    'Decimal(9,2)', 'Decimal(38,10)',
    'String', 'FixedString(5)', 'Enum8(''a'' = 1)',
    'Date', 'Date32', 'DateTime', 'DateTime64(3)',
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

SELECT pgch_roundtrip('Bool', true), pgch_roundtrip('Bool', false);

SELECT pgch_roundtrip('Float32', 1.5::float4),
       pgch_roundtrip('Float64', (-2.25)::float8);

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
SELECT encode(pgch_encode('String', '\x00ff'::bytea), 'hex'),
       encode(pgch_encode('FixedString(5)', 'abc'::text), 'hex');

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
       pgch_roundtrip('FixedString(4)', 'fs'::varchar),
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
SELECT encode(pgch_encode_rows('Int32', ARRAY[1, 2, 3]::int4[]), 'hex');

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
-- Nest tuples through the cursor, Nullable wrapping the inner one
SELECT pgch_decode(pgch_encode_pairs('Map(String, Tuple(Int64))',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[],
                                     2, true)) AS nested,
       pgch_decode(pgch_encode_pairs('Array(Tuple(String, Nullable(Tuple(Int64))))',
                                     ARRAY['a', 'b'], ARRAY[1, 2]::bigint[],
                                     2, true)) AS nullable_nested;
