# chDB integration

Use Native format to stream data between chDB and PostgreSQL.
Match writer types to declared ClickHouse table structure, including arrays
and other nested types.

## Write rows

Open an insert stream with `chdb_stream_insert(conn, query, "Native")`.
Append rows with `pgch_append_datum` or `pgch_append_slot`. Use
`pgch_writer_rows` or `pgch_writer_bytes` to decide when to send a block:

```c
pgch_buf buf = {};

pgch_writer_flush(writer, &buf, NULL);
if (chdb_stream_append(stream, (char *) buf.data, buf.len) != CHDBSuccess)
    ereport(ERROR, ...);
pgch_buf_reset(&buf);
```

## Read rows

Apply `PGCH_NATIVE_SETTINGS` to queries returning Native data. Wrap
`chdb_stream_fetch_result` in a
[chunk source](pg-clickhouse-decode.md#supply-native-byte-chunks), keeping each
result buffer alive until next chunk request. Use `pgch_reader_init_chunks`
with `NULL` options for local framing.

Read rows and convert them to target PostgreSQL types with
[`pgch_reader_fill`](pg-clickhouse-decode.md#complete-reader-example).

See [pg_chdb](https://github.com/ClickHouse/pg_chdb) for query execution,
streaming, and PostgreSQL COPY integration.
