-- Map ClickHouse declarations to PostgreSQL column descriptors
SET TimeZone = 'UTC';

-- Scalar mappings carry no modifier
SELECT t AS ch_type, c.type, c.ndims, c.nullable, c.is_column
  FROM unnest(ARRAY[
    'Int8', 'Int16', 'Int32', 'Int64',
    'UInt8', 'UInt16', 'UInt32', 'UInt64',
    'Bool', 'Float32', 'Float64', 'BFloat16',
    'String', 'FixedString(5)', 'Enum8(''a'' = 1)', 'Enum16(''a'' = 1)',
    'UUID', 'IPv4', 'IPv6', 'JSON', 'Object(''json'')',
    'Date', 'Date32', 'DateTime', 'DateTime(''Europe/Berlin'')', 'Time',
    'IntervalNanosecond', 'IntervalDay', 'IntervalYear'
]) AS t, pgch_pgcolumn(t) AS c;

-- Nothing describes no column, so it maps to no type
SELECT t AS ch_type, c.type, c.ndims, c.nullable, c.is_column
  FROM unnest(ARRAY['Nothing', 'Nullable(Nothing)']) AS t, pgch_pgcolumn(t) AS c;
SELECT pgch_pgcolumn('Array(Nothing)');

-- Geometric types map onto their PostgreSQL counterparts, multi-forms as arrays
SELECT t AS ch_type, c.type, c.ndims, c.nullable, c.is_column
  FROM unnest(ARRAY[
    'Point', 'Ring', 'LineString', 'Polygon', 'MultiPolygon', 'MultiLineString'
]) AS t, pgch_pgcolumn(t) AS c;

-- Decimal keeps precision and scale, width defaults come from the declaration
SELECT t AS ch_type, c.type
  FROM unnest(ARRAY[
    'Decimal', 'Decimal(9,2)', 'Decimal(76,20)',
    'Decimal32(4)', 'Decimal64(6)', 'Decimal128(10)', 'Decimal256(30)'
]) AS t, pgch_pgcolumn(t) AS c;

-- Fractional-second precision caps at the PostgreSQL maximum
SELECT t AS ch_type, c.type, c.truncated
  FROM unnest(ARRAY[
    'DateTime64(0)', 'DateTime64(3)', 'DateTime64(6)', 'DateTime64(9)',
    'DateTime64(3, ''Europe/Berlin'')',
    'Time64(0)', 'Time64(3)', 'Time64(6)', 'Time64(9)',
    'IntervalNanosecond', 'IntervalMicrosecond', 'IntervalDay',
    'Nullable(IntervalNanosecond)', 'Array(IntervalNanosecond)'
]) AS t, pgch_pgcolumn(t) AS c;

-- Only an outer Nullable makes the column nullable
SELECT t AS ch_type, c.type, c.nullable
  FROM unnest(ARRAY[
    'Nullable(String)', 'LowCardinality(String)',
    'LowCardinality(Nullable(String))',
    'Nullable(Decimal(12,6))', 'LowCardinality(Nullable(FixedString(4)))',
    'Nullable(DateTime64(9))', 'Nullable(Array(Int32))',
    'Array(Nullable(Int32))', 'Array(LowCardinality(Nullable(String)))'
]) AS t, pgch_pgcolumn(t) AS c;

-- One PostgreSQL array type serves every ClickHouse nesting depth, with typmod
SELECT t AS ch_type, c.type, c.ndims, c.nullable, c.truncated, c.is_column
  FROM unnest(ARRAY[
    'Array(Int32)', 'Array(Array(String))', 'Array(Array(Array(Int64)))',
    'Array(Decimal(12,6))', 'Array(Array(Decimal(12,6)))',
    'Array(Nullable(Decimal(9,4)))',
    'Array(DateTime64(3))', 'Array(Nullable(DateTime64(9)))',
    'Array(Time64(6))', 'Array(Nullable(Time64(9)))',
    'Array(Point)', 'Array(Tuple(Int32, String))',
    'Array(Polygon)', 'Array(MultiPolygon)', 'Array(Map(String, Int64))'
]) AS t, pgch_pgcolumn(t) AS c;

-- Tuple and Map reach PostgreSQL pseudotypes no table column can hold
SELECT t AS ch_type, c.type, c.ndims, c.is_column
  FROM unnest(ARRAY[
    'Tuple(Int32, String)', 'Tuple(a Int32, b String)',
    'Map(String, Int64)', 'Map(String, Array(Decimal(12,6)))'
]) AS t, pgch_pgcolumn(t) AS c;

-- FixedString counts bytes, which varchar(N) cannot express, so it maps to text
SELECT t AS ch_type, c.type, c.ndims
  FROM unnest(ARRAY[
    'FixedString(1)', 'FixedString(50)', 'Nullable(FixedString(50))',
    'LowCardinality(FixedString(8))',
    'Array(FixedString(3))', 'Array(Array(FixedString(3)))'
]) AS t, pgch_pgcolumn(t) AS c;

-- Types PostgreSQL cannot represent report the ClickHouse declaration
SELECT pgch_pgcolumn('Int128');
SELECT pgch_pgcolumn('UInt256');
SELECT pgch_pgcolumn('Dynamic');
SELECT pgch_pgcolumn('Variant(Int32, String)');
SELECT pgch_pgcolumn('QBit(Float32, 16)');
SELECT pgch_pgcolumn('Array(Int128)');
SELECT pgch_pgcolumn('AggregateFunction(sum, Int64)');

-- Malformed declarations report the clickhouse-c parse error
SELECT pgch_pgcolumn('Nonsense');
SELECT pgch_pgcolumn('Int32junk');
SELECT pgch_pgcolumn('Array(Int32');
SELECT pgch_pgcolumn('Decimal(9');
SELECT pgch_pgcolumn('');
