-- Map and encode pgvector vector values as ClickHouse float arrays
CREATE EXTENSION IF NOT EXISTS vector;

-- vector maps to Array(Float32) through domains and as an array element
CREATE DOMAIN vec3 AS vector(3);
SELECT pgch_chtype('vector'),
       pgch_chtype('vector(3)', true),
       pgch_chtype('vec3'),
       pgch_chtype('vector[]');

-- Other types named vector keep their own mapping
CREATE SCHEMA notpgvector;
CREATE TYPE notpgvector.vector AS (x int);
SELECT pgch_chtype('notpgvector.vector'), pgch_chtype('halfvec');

-- Write each float element width, reading back as real[] and as vector
-- BFloat16 keeps these values exactly
SELECT pgch_roundtrip('Array(Float32)', '[1,2.5,-3]'::vector),
       pgch_roundtrip('Array(Float64)', '[0.1,2.5,-3]'::vector),
       pgch_roundtrip('Array(BFloat16)', '[0.5,1.25,-2]'::vector);

SELECT pgch_roundtrip_as('Array(Float32)', '[0.1,0.2,0.3]'::vector),
       pgch_roundtrip_as('Array(Float64)', '[0.1,0.2,0.3]'::vector),
       pgch_roundtrip_as('Array(BFloat16)', '[0.5,1.25,-2]'::vector);

-- Vectors fill QBit columns, reading back as vector
SELECT pgch_roundtrip_as('QBit(Float32, 3)', '[0.1,0.2,0.3]'::vector),
       pgch_roundtrip_as('QBit(BFloat16, 3)', '[0.5,1.25,-2]'::vector),
       pgch_roundtrip_as('QBit(Float64, 3)', '[0.1,0.2,0.3]'::vector);
SELECT pgch_roundtrip('Array(QBit(Float32, 2))', ARRAY['[1,2]', '[3,4]']::vector[]);
SELECT pgch_roundtrip_rows('Nullable(QBit(Float64, 2))',
                          ARRAY['[1,2]', NULL, '[3,4]']::vector[]);

-- Arrays of vectors fill nested arrays, one vector per inner array
SELECT pgch_roundtrip('Array(Array(Float32))', ARRAY['[1,2]', '[3,4]']::vector[]);

-- Several rows reuse the column's conversion
SELECT pgch_roundtrip_rows('Array(Float64)', ARRAY['[1,2]', '[3,4]']::vector[]);

DROP TYPE notpgvector.vector;
DROP SCHEMA notpgvector;
DROP DOMAIN vec3;
