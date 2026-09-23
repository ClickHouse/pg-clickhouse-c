-- Build decoder and encoder type tables in doc/
-- gen_type_table.awk splices the marked block of this test's expected
-- output into documentation, so the table needs no database to regenerate
--
-- Probe every type name the parser resolves
-- New types reach the table or omitted list automatically
--
-- psql aligned output with border 2 pads the columns markdown wants
\pset border 2
\pset footer off

SELECT r[1] AS "Omitted", r[2] AS "Reason" FROM pgch_rows(pgch_type_omitted()) t(r);

\echo TYPE-TABLE-BEGIN
SELECT r[1] AS "ClickHouse", r[2] AS "Default PostgreSQL",
       r[3] AS "Additional read targets", r[4] AS "Notes"
    FROM pgch_rows(pgch_type_table()) t(r);
\echo TYPE-TABLE-END

\echo ENCODE-TABLE-BEGIN
SELECT pg AS "PostgreSQL",
       pgch_chtype(CASE WHEN pg = 'oid8'
                       THEN coalesce(to_regtype('oid8'), 'xid8'::regtype)::text
                       ELSE pg END, true) AS "Default ClickHouse",
       behavior AS "Notes"
FROM (VALUES
    ('boolean', ''),
    ('smallint', ''),
    ('integer', ''),
    ('bigint', ''),
    ('oid', ''),
    ('xid8', ''),
    ('oid8', ''),
    ('real', 'BFloat16 drops low mantissa bits'),
    ('double precision', ''),
    ('numeric', 'numeric_as_string selects String'),
    ('numeric(12,6)', 'Precision selects Decimal width'),
    ('text', 'low_cardinality selects LowCardinality(String)'),
    ('bytea', 'Writes raw bytes'),
    ('date', ''),
    ('time', ''),
    ('timestamp', ''),
    ('timestamptz', ''),
    ('interval', 'Interval destinations require whole unit counts'),
    ('uuid', ''),
    ('json', 'json_as_json selects JSON'),
    ('jsonb', 'json_as_json selects JSON'),
    ('inet', 'Override with IPv4 or IPv6 matching address family'),
    ('point', ''),
    ('lseg', 'Two points'),
    ('path', 'Closed paths repeat the first point'),
    ('polygon', ''),
    ('box', ''),
    ('circle', ''),
    ('line', '')
) AS types(pg, behavior);
\echo ENCODE-TABLE-END

\pset border 1
\pset footer on
