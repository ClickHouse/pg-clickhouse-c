\unset ECHO
/*
 * This test must be run in a database with UTF-8 or SQL_ASCII encoding, as
 * we have expected output files only for thowse encodings.
 */
SELECT getdatabaseencoding() NOT IN ('UTF8', 'SQL_ASCII', 'EUC_KR') AS skip_test
  FROM pg_database
 WHERE datname=current_database() \gset
\if :skip_test
\echo 'SKIP: can only test UTF8, SQL_ASCII EUC_KR encodings'
\quit
\endif
\set ECHO all

-- Treat invalid encoding according to flag: truncate, remove, and replace.
SELECT description,
       pgch_decode_text(pgch_encode('String',input), 1) AS truncate,
       pgch_decode_text(pgch_encode('String',input), 2) AS remove,
       pgch_decode_text(pgch_encode('String',input), 3) AS replace
FROM (VALUES
     ('valid'::text, '\x61 63 6e'::bytea),
     ('nul byte', '\x61   00   6e'),
     ('nul & invalid octet', '\x61   00   6e   80   6f'),
     ('invalid octet & nul', '\x61 6e   80   6f   00  '),
     ('valid 2-octet sequence', '\x 61   c3 b1   6e'),
     ('invalid 2-octet sequence', '\x61   c3 28   6e'),
     ('valid 3-octet sequence', '\x 61   e2 82 a1   6e'),
     ('invalid 2nd in 3-octet sequence', '\x61   e2 28 a1   6e'),
     ('invalid 3rd in 3-octet sequence', '\x61   e2 82 28   6e'),
     ('valid 4-octet sequence', '\x61 63 6e   f0 90 8c bc   61 63 6e'),
     ('invalid 2nd in 4-octet sequence', '\x61 63 6e  f0 28 8c bc  61 63 6e'),
     ('invalid 3nd in 4-octet sequence', '\x61 63 6e  f0 90 28 bc  61 63 6e'),
     ('invalid 4th in 4-octet sequence', '\x61 63 6e  f0 28 8c 28  61 63 6e')
) x(description, input);

-- Check output length for repeated invalid bytes under each encoding policy
SELECT length((pgch_decode_text(pgch_encode('String', input), 1))[1]) AS truncate,
       length((pgch_decode_text(pgch_encode('String', input), 2))[1]) AS remove,
       length((pgch_decode_text(pgch_encode('String', input), 3))[1]) AS replace
  FROM (SELECT decode(repeat('61c3286e', 64), 'hex')) x(input);

-- Apply encoding policy when pgch_decode renders FixedString values as text
SELECT description,
       pgch_decode(pgch_encode('FixedString(5)', input), 1) AS truncate,
       pgch_decode(pgch_encode('FixedString(5)', input), 2) AS remove,
       pgch_decode(pgch_encode('FixedString(5)', input), 3) AS replace
FROM (VALUES
     ('padding'::text, '\x61 62'::bytea),
     ('padding & invalid octet', '\x61 80 62')
) x(description, input);

-- Allow trailing NUL padding in FixedString even when invalid bytes cause errors
SELECT pgch_decode(pgch_encode('FixedString(5)', '\x6162'::bytea)) AS fail;

-- Validate the encoding_check parser.
SELECT x, pgch_encoding_check_enum(x::text) AS num
FROM   (
     VALUES ('fail'), ('replace'), ('truncate'), ('remove'), ('nonesuch'),
            ('Fail'), ('TRUNCATE'), ('rePlaCe'), ('removE'), ('noneSuch')
) x(x);
