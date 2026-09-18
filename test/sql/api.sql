-- Verify guards on calls the library's own paths never make
SET lc_messages = 'C';

-- Report columns past the writer instead of reading past them
SELECT pgch_writer_probe('column_range') AS out_of_range,
       pgch_writer_probe('array_scale') AS array_scale,
       pgch_writer_probe('close_idle') AS close_idle;
SELECT pgch_writer_probe('array_range');
SELECT pgch_writer_probe('nesting');
SELECT pgch_writer_probe('append_range');
SELECT pgch_writer_probe('unnamed');
SELECT pgch_writer_probe('tuple_null');

-- Save and restore row position across column layouts
SELECT pgch_writer_probe('checkpoint_grow') AS grow,
       pgch_writer_probe('rollback_built') AS rollback_built,
       pgch_writer_probe('rebuild') AS rebuild,
       pgch_writer_probe('bytes') AS bytes;
SELECT pgch_writer_probe('checkpoint_nested');
SELECT pgch_writer_probe('checkpoint_stale');

-- Reject ClickHouse types the parser cannot spell
SELECT pgch_writer_probe('interval_unit');
SELECT pgch_writer_probe('datetime64_scale');
SELECT pgch_reader_probe('read_datetime64_scale');
SELECT pgch_reader_probe('read_time64_scale');
SELECT pgch_reader_probe('read_unsupported');
SELECT pgch_reader_probe('read_array_item');

-- Release a reader before its block runs out
SELECT pgch_decode_first(pgch_encode_rows('Int32', ARRAY[1, 2, 3]::int4[])) AS first,
       pgch_decode_first(pgch_encode_rows('Int32', ARRAY[]::int4[])) AS empty;

-- Report a source that fails before the first block
SELECT pgch_decode_failed_source();

-- Prepare conversion from a declaration rather than from the block
SELECT pgch_decode_typed_decl(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]),
                              'Int32', NULL::int8) AS widened,
       pgch_decode_typed_decl(pgch_encode_rows('Nullable(Int32)',
                                               ARRAY[1, NULL]::int4[]),
                              'Nullable(Int32)', NULL::text) AS text;

-- Stop chunk delivery mid-stream, with and without cancellation
SELECT pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]), 4,
                          fail_at => 1);
SELECT pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]), 4,
                          fail_at => 2);
SELECT pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]), 4,
                          cancel_at => 1);
SELECT pgch_decode_chunks(pgch_encode_rows('Int32', ARRAY[1, 2]::int4[]), 4,
                          fail_at => -1);

-- Reject columns whose shape the wire cannot spell
SELECT pgch_reader_probe('read_empty_tuple');
SELECT pgch_reader_probe('read_lc_key_size');
SELECT pgch_reader_probe('read_lc_inner');
SELECT pgch_reader_probe('read_decimal_width');
SELECT pgch_reader_probe('read_wide_width');
SELECT pgch_reader_probe('read_decimal_scale');
SELECT pgch_reader_probe('read_many_points');

-- Keep callbacks terminal after EOF and source failure
SELECT pgch_reader_probe('chunk_eof');
SELECT pgch_reader_probe('chunk_error');

-- Reject valid types exceeding Native writer's formatting buffer
SELECT pgch_writer_probe('flush_long_type');

-- Fail each allocation in turn, recover and compare complete results
SELECT pgch_fault_probe('append');
SELECT pgch_fault_probe('flush');
SELECT pgch_fault_probe('read');
