-- Datum -> Native -> Datum round trips, entirely in memory.
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';

-- CH type -> PG type mapping.
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

-- Integers, including the widening CH does not have a PG twin for.
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

-- Decimals: 32/64 take the int64 fast path, 128/256 the digit-string path.
SELECT pgch_roundtrip('Decimal(9,2)', 123.45::numeric),
       pgch_roundtrip('Decimal(9,2)', (-123.45)::numeric),
       pgch_roundtrip('Decimal(18,6)', 1.000001::numeric),
       pgch_roundtrip('Decimal(38,10)', 12345678901234567890.0123456789::numeric),
       pgch_roundtrip('Decimal(76,20)', (-0.00000000000000000001)::numeric);

SELECT pgch_roundtrip('String', 'hello'::text),
       pgch_roundtrip('String', ''::text),
       pgch_roundtrip('String', E'tab\there'::text),
       pgch_roundtrip('FixedString(5)', 'abcde'::text);

-- bytea goes out verbatim, FixedString right-pads with NULs: assert the wire
-- bytes, since a decoded value with embedded NULs cannot survive a cstring.
SELECT encode(pgch_encode('String', '\x00ff'::bytea), 'hex'),
       encode(pgch_encode('FixedString(5)', 'abc'::text), 'hex');

SELECT pgch_roundtrip('Enum8(''red'' = 1, ''green'' = 2)', 'green'::text),
       pgch_roundtrip('Enum16(''a'' = -300, ''b'' = 300)', 'a'::text);

-- Dates and times.
SELECT pgch_roundtrip('Date', '2024-01-15'::date),
       pgch_roundtrip('Date32', '2024-01-15'::date),
       pgch_roundtrip('Date32', '1950-03-04'::date);

SELECT pgch_roundtrip('DateTime', '2024-01-15 12:34:56+00'::timestamptz),
       pgch_roundtrip('DateTime64(3)', '2024-01-15 12:34:56.123+00'::timestamptz),
       pgch_roundtrip('DateTime64(6)', '2024-01-15 12:34:56.123456+00'::timestamptz),
       pgch_roundtrip('DateTime64(9)', '2024-01-15 12:34:56.123456+00'::timestamptz);

SELECT pgch_roundtrip('UUID', '11111111-2222-3333-4444-555555555555'::uuid),
       pgch_roundtrip('IPv4', '192.168.1.1'::inet),
       pgch_roundtrip('IPv6', '2001:db8::1'::inet);

SELECT pgch_roundtrip('JSON', '{"a": [1, 2], "b": null}'::jsonb);

-- Nullability, including the Nullable-inside-LowCardinality shape.
SELECT pgch_roundtrip('Nullable(Int32)', NULL::int4) IS NULL AS null_int,
       pgch_roundtrip('Nullable(String)', NULL::text) IS NULL AS null_text,
       pgch_roundtrip('Nullable(Int32)', 7::int4);

SELECT pgch_roundtrip_rows('Nullable(Int32)', ARRAY[1, NULL, 3]::int4[]),
       pgch_roundtrip_rows('LowCardinality(String)', ARRAY['a', 'b', 'a']::text[]),
       pgch_roundtrip_rows('LowCardinality(Nullable(String))',
                           ARRAY['a', NULL, 'a', 'b']::text[]);

-- Arrays: the shapes TSV cannot carry.
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

-- Multiple rows in one block, and multiple blocks in one stream.
SELECT pgch_roundtrip_rows('Int32', ARRAY[1, 2, 3]::int4[]);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]) ||
                   pgch_encode_rows('Int32', ARRAY[3]::int4[]));

-- Zero rows still carries a schema.
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[]::int4[]));

-- Wire layout is pinned: 1 column, 3 rows, "c", "Int32", 3 LE int32.
SELECT encode(pgch_encode_rows('Int32', ARRAY[1, 2, 3]::int4[]), 'hex');

-- Tuple decodes into a record; the encoder has no PG composite path.
SELECT pgch_decode(
    '\x0102'::bytea ||
    '\x01' || convert_to('c', 'UTF8') ||
    '\x14' || convert_to('Tuple(Int32, String)', 'UTF8') ||
    '\x2a000000ffffffff'::bytea ||
    '\x02' || convert_to('hi', 'UTF8') ||
    '\x03' || convert_to('bye', 'UTF8')
);
