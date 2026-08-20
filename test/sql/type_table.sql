-- Build the ClickHouse to PostgreSQL type table of README
-- gen_type_table.awk splices the marked block of this test's expected
-- output into README, so the table needs no database to regenerate
--
-- Probe every type name the parser resolves
-- New types reach the table or omitted list automatically
--
-- psql aligned output with border 2 pads the columns markdown wants
\pset border 2
\pset footer off

SELECT r[1] AS "Omitted", r[2] AS "Reason" FROM pgch_rows(pgch_type_omitted()) t(r);

\echo TYPE-TABLE-BEGIN
SELECT r[1] AS "ClickHouse", r[2] AS "PostgreSQL", r[3] AS "Notes"
    FROM pgch_rows(pgch_type_table()) t(r);
\echo TYPE-TABLE-END

\pset border 1
\pset footer on
