-- Verify PostgreSQL to ClickHouse declarations and String conversions
SET lc_monetary = 'C';
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';
SET IntervalStyle = 'postgres';

-- Verify default type mapping, array elements nullable under either flag
SELECT d AS pg_type, pgch_chtype(d, true) AS notnull, pgch_chtype(d) AS nullable
FROM unnest(ARRAY[
    'bool', 'int2', 'int4', 'int8', 'oid', 'xid8', 'float4', 'float8',
    'numeric', 'numeric(12,6)', 'numeric(9)', 'numeric(80,2)',
    'text', 'varchar(8)', 'bpchar(6)', 'name', '"char"', 'bytea',
    'date', 'time', 'timestamp', 'timestamptz', 'timetz', 'interval',
    'uuid', 'json', 'jsonb',
    'inet', 'cidr', 'macaddr', 'macaddr8', 'bit(4)', 'varbit(6)',
    'point', 'lseg', 'path', 'polygon', 'box', 'circle', 'line',
    'money', 'tsvector', 'tsquery', 'jsonpath',
    'int4[]', 'text[]', 'numeric(12,6)[]', 'interval[]',
    'polygon[]', 'box[]'
]) AS d;

-- Verify optional JSON, LowCardinality, and numeric mappings
SELECT pgch_chtype('jsonb', json_as_json => true) AS json_null,
       pgch_chtype('jsonb', true, json_as_json => true) AS json_notnull,
       pgch_chtype('text', low_cardinality => true) AS lc_null,
       pgch_chtype('text', true, low_cardinality => true) AS lc_notnull,
       pgch_chtype('numeric', numeric_as_string => true) AS num_string;

-- Map domains through base type and typmod
CREATE DOMAIN dcount AS int4;
CREATE DOMAIN dvc AS varchar(8);
SELECT pgch_chtype('dcount', true), pgch_chtype('dvc', true), pgch_chtype('dcount[]', true);

-- Decode generated ClickHouse declarations into compatible PostgreSQL types
SELECT d AS pg_type, c AS ch_type, pgch_pgtype(c) AS decodes_as
FROM (SELECT d, pgch_chtype(d, true) AS c FROM unnest(ARRAY[
    'bool', 'int2', 'int4', 'int8', 'oid', 'xid8', 'float4', 'float8',
    'numeric(12,6)', 'text', 'date', 'time', 'timestamptz', 'uuid',
    'int4[]', 'text[]',
    'point', 'lseg', 'path', 'polygon', 'box', 'circle', 'line', 'polygon[]'
]) AS d) q;

-- Round-trip PostgreSQL types represented by ClickHouse String
CREATE FUNCTION rt(lit text, typ text, ch text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE res text;
BEGIN
    EXECUTE format('SELECT pgch_roundtrip_as(%L, %s::%s)', ch, lit, typ) INTO res;
    RETURN res;
END $$;

CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');

SELECT typ, rt(lit, typ, ch) AS scalar, rt(arr, typ || '[]', 'Array(' || ch || ')') AS array
FROM (VALUES
    ('''1 day 02:03:04''',      'ARRAY[''1 day'', ''2 hours'']',      'interval', 'String'),
    ('''12:34:56+02''',         'ARRAY[''12:34:56+02'']',             'timetz',   'String'),
    ('''a b''',                 'ARRAY[''a b'']',                     'tsvector', 'String'),
    ('''a & b''',               'ARRAY[''a & b'']',                   'tsquery',  'String'),
    ('''$.a[0]''',              'ARRAY[''$.a[0]'']',                  'jsonpath', 'String'),
    ('''3.00''',                'ARRAY[''3.00'', ''-4.50'']',         'money',    'String'),
    ('''08:00:2b:01:02:03''',   'ARRAY[''08:00:2b:01:02:03'']',       'macaddr',  'String'),
    ('''08:00:2b:01:02:03:04:05''', 'ARRAY[''08:00:2b:01:02:03:04:05'']', 'macaddr8', 'String'),
    ('''10.0.0.0/8''',          'ARRAY[''10.0.0.0/8'', ''::1/128'']',  'cidr',     'String'),
    ('B''1010''',               'ARRAY[B''1010'']',                   'bit(4)',   'String'),
    ('B''101''',                'ARRAY[B''101'']',                    'varbit',   'String'),
    ('''(1,2)''',               'ARRAY[''(1,2)'']',                   'point',    'String'),
    ('''((0,0),(1,1),(2,0))''', 'ARRAY[''((0,0),(1,1))'']',           'polygon',  'String'),
    ('''<(1,2),3>''',           'ARRAY[''<(1,2),3>'']',               'circle',   'String'),
    ('''ok''',                  'ARRAY[''ok'', ''sad'']',             'mood',     'String'),
    ('''ok''',                  'ARRAY[''ok'', ''sad'']',             'mood',     'LowCardinality(String)')
) v(lit, arr, typ, ch);

-- Prepare conversion from schema before nullable rows
SELECT pgch_decode_typed(pgch_encode_rows('Nullable(String)',
                                          ARRAY[NULL, '1 day']::text[]),
                         NULL::interval),
       pgch_decode_typed(pgch_encode_rows('Nullable(String)',
                                          ARRAY[NULL, NULL]::text[]),
                         NULL::interval),
       pgch_decode_typed(pgch_encode('Array(Int64)', ARRAY[1, 2]::int8[]),
                         NULL::int4[]);

-- Convert array elements into requested PostgreSQL type
SELECT pgch_decode_as(pgch_encode('Array(Int64)', ARRAY[1, 2, 3]::int8[]), NULL::int4[]),
       pgch_decode_as(pgch_encode('Array(Array(Int64))', ARRAY[[1, 2], [3, 4]]::int8[]),
                      NULL::int4[]),
       pgch_decode_as(pgch_encode('Array(Nullable(String))',
                                  ARRAY['1 day', NULL]::text[]), NULL::interval[]);

-- Convert arrays into array domains
CREATE DOMAIN dints AS int4[];
SELECT pgch_decode_as(pgch_encode('Array(Int32)', ARRAY[1, 2]::int4[]), NULL::dints),
       pgch_decode_as(pgch_encode('Array(Int64)', ARRAY[1, 2]::int8[]), NULL::dints);

-- Apply target domain constraints after conversion
CREATE DOMAIN dpos AS int4 CHECK (VALUE > 0);
CREATE DOMAIN dposints AS int4[] CHECK (VALUE[1] > 0);
SELECT pgch_decode_as(pgch_encode('Int32', 2::int4), NULL::dpos),
       pgch_decode_as(pgch_encode('Int64', 3::int8), NULL::dpos),
       pgch_decode_as(pgch_encode('String', '4'::text), NULL::dpos),
       pgch_decode_as(pgch_encode('Array(Int32)', ARRAY[5, 6]::int4[]), NULL::dposints);

-- Enforce array type modifier on each element, as ArrayCoerceExpr does
CREATE DOMAIN dnums AS numeric(4, 1)[];
CREATE DOMAIN dchars AS bpchar(3)[];
SELECT pgch_decode_as(pgch_encode('Array(Decimal(10, 4))',
                                  ARRAY[1.2345, -6.789]::numeric[]), NULL::dnums),
       pgch_decode_as(pgch_encode('Array(Nullable(Int64))',
                                  ARRAY[12, NULL]::int8[]), NULL::dnums),
       pgch_decode_as(pgch_encode('Array(String)',
                                  ARRAY['ab', 'cd']::text[]), NULL::dchars);

-- Take the type modifier from the call site, as arguments carry none
SELECT pgch_decode_as(pgch_encode('Array(Decimal(10, 4))',
                                  ARRAY[1.2345, -6.789]::numeric[]),
                      NULL::numeric(4,1)[]),
       pgch_decode_typed(pgch_encode('Array(String)',
                                     ARRAY['ab', 'cd']::text[]), NULL::bpchar(3)[]),
       pgch_decode_as(pgch_encode('String', 'abc'::text), NULL::varchar(4));

-- Render scalars into a string type through the output function
SELECT pgch_decode_as(pgch_encode('Int8', 7::int2), NULL::text),
       pgch_decode_as(pgch_encode('Float64', 1.5::float8), NULL::text),
       pgch_decode_as(pgch_encode('DateTime', '2024-01-15 12:34:56+00'::timestamptz),
                      NULL::text),
       pgch_decode_as(pgch_encode('Nullable(Int32)', NULL::int4), NULL::text);

-- Arrays hold no built value to render, so a string target stays rejected
SELECT pgch_decode_as(pgch_encode('Array(Int64)', ARRAY[1, 2]::int8[]), NULL::text);

-- Round-trip relation through generated structure and slot API
CREATE TABLE copyshape (
    id      int          NOT NULL,
    name    varchar(16),
    amount  numeric(9,2),
    ival    interval,
    mac     macaddr,
    ip      inet,
    when_   timestamptz,
    day     date,
    tags    text[],
    counts  int8[]       NOT NULL,
    doc     jsonb
);
INSERT INTO copyshape VALUES
    (1, 'first', 12.34, '1 day 02:03:04', '08:00:2b:01:02:03', '10.0.0.1',
     '2024-01-15 12:34:56.123456+00', '2024-01-15', ARRAY['a', 'b c'],
     ARRAY[1, 2, 3], '{"a": [1, 2]}'),
    (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, ARRAY[]::int8[], NULL);

SELECT pgch_structure('copyshape');
SELECT unnest(pgch_table_roundtrip('copyshape', null_array_empty => true));

-- Generate structure with optional mappings
SELECT pgch_structure('copyshape', json_as_json => true, low_cardinality => true);

-- Skip dropped columns consistently
ALTER TABLE copyshape DROP COLUMN mac;
SELECT pgch_structure('copyshape');
SELECT unnest(pgch_table_roundtrip('copyshape', null_array_empty => true));

-- Carry a NULL ring or line as the empty one, which PostgreSQL cannot spell
CREATE TABLE geoshape (poly polygon, p path, ls lseg, b box, polys polygon[]);
INSERT INTO geoshape VALUES
    ('((0,0),(1,1),(2,0))', '[(0,0),(1,1)]', '((1,1),(2,2))', '((1,1),(3,3))',
     ARRAY['((0,0),(1,1),(2,0))', NULL]::polygon[]),
    (NULL, NULL, NULL, NULL, NULL);

SELECT pgch_structure('geoshape');
SELECT unnest(pgch_table_roundtrip('geoshape', null_array_empty => true));

-- Keep non-finite values, which Float columns take
SELECT pgch_roundtrip('Float64', 'NaN'::float8) AS float_keeps_nan;
SELECT pgch_roundtrip('Float32', '-Infinity'::float4) AS float_keeps_inf;

-- Return required Native query settings
SELECT pgch_native_settings();
