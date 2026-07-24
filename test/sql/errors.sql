-- Rejections. Every message carries the pgch_msg_prefix _PG_init set.
SET lc_messages = 'C';

-- NULL into a column ClickHouse declared NOT NULL.
SELECT pgch_roundtrip('Int32', NULL::int4);
SELECT pgch_roundtrip('Array(Int32)', NULL::int4[]);
SELECT pgch_roundtrip('LowCardinality(String)', NULL::text);

-- PG type the destination column cannot hold.
SELECT pgch_roundtrip('Int32', 'x'::text);
SELECT pgch_roundtrip('Array(Int32)', 1::int4);
SELECT pgch_roundtrip('IPv4', '::1'::inet);

-- Values the destination cannot represent.
SELECT pgch_roundtrip('Decimal(9,2)', 'NaN'::numeric);
SELECT pgch_roundtrip('Enum8('' a'' = 1)', 'z'::text);

-- Types the encoder has no buffer shape for.
SELECT pgch_encode('Tuple(Int32)', ROW(1)::record);
SELECT pgch_encode('LowCardinality(Int32)', 1::int4);

-- Types with no PG mapping.
SELECT pgch_pgtype('Int128');
SELECT pgch_pgtype('Map(String, Int32)');
SELECT pgch_pgtype('Nonsense');

-- Truncated and drifting streams.
SELECT pgch_decode('\x0103'::bytea);
SELECT pgch_decode(pgch_encode_rows('Int32', ARRAY[1]::int4[]) ||
                   pgch_encode_rows('String', ARRAY['a']::text[]));
