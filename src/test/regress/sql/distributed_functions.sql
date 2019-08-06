--
-- Regression tests for deparsing ALTER/DROP TABLE Queries
--
-- This test implements all the possible queries as of Postgres 11:
-- 
-- ALTER FUNCTION name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ]
--     action [ ... ] [ RESTRICT ]
-- ALTER FUNCTION name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ]
--     RENAME TO new_name
-- ALTER FUNCTION name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ]
--     OWNER TO { new_owner | CURRENT_USER | SESSION_USER }
-- ALTER FUNCTION name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ]
--     SET SCHEMA new_schema
-- ALTER FUNCTION name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ]
--     DEPENDS ON EXTENSION extension_name
-- 
-- where action is one of:
-- 
--     CALLED ON NULL INPUT | RETURNS NULL ON NULL INPUT | STRICT
--     IMMUTABLE | STABLE | VOLATILE | [ NOT ] LEAKPROOF
--     [ EXTERNAL ] SECURITY INVOKER | [ EXTERNAL ] SECURITY DEFINER
--     PARALLEL { UNSAFE | RESTRICTED | SAFE }
--     COST execution_cost
--     ROWS result_rows
--     SET configuration_parameter { TO | = } { value | DEFAULT }
--     SET configuration_parameter FROM CURRENT
--     RESET configuration_parameter
--     RESET ALL
-- 
-- DROP FUNCTION [ IF EXISTS ] name [ ( [ [ argmode ] [ argname ] argtype [, ...] ] ) ] [, ...]
--     [ CASCADE | RESTRICT ]

SET citus.next_shard_id TO 20020000;

CREATE SCHEMA function_tests;
SET search_path TO function_tests;
SET citus.shard_count TO 4;
SET client_min_messages TO DEBUG;

CREATE VIEW all_functions AS
SELECT p.proname as "Name",
       pg_catalog.pg_get_function_result(p.oid) as "Result data type",
       pg_catalog.pg_get_function_arguments(p.oid) as "Argument data types"
FROM pg_catalog.pg_proc p
     LEFT JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace
WHERE n.nspname = 'function_tests'
ORDER BY 1, 2, 3;

-- Create a simple function
CREATE FUNCTION add(integer, integer) RETURNS integer
    AS 'select $1 + $2;'
    LANGUAGE SQL
    IMMUTABLE
    RETURNS NULL ON NULL INPUT;

ALTER FUNCTION add CALLED ON NULL INPUT;
ALTER FUNCTION add RETURNS NULL ON NULL INPUT;
ALTER FUNCTION add STRICT;

ALTER FUNCTION add IMMUTABLE;
ALTER FUNCTION add STABLE;
ALTER FUNCTION add VOLATILE;
ALTER FUNCTION add LEAKPROOF;
ALTER FUNCTION add NOT LEAKPROOF;

ALTER FUNCTION add EXTERNAL SECURITY INVOKER;
ALTER FUNCTION add SECURITY INVOKER;
ALTER FUNCTION add EXTERNAL SECURITY DEFINER;
ALTER FUNCTION add SECURITY DEFINER;

ALTER FUNCTION add PARALLEL UNSAFE;
ALTER FUNCTION add PARALLEL RESTRICTED;
ALTER FUNCTION add PARALLEL SAFE;

-- The COST/ROWS arguments should always be numeric
ALTER FUNCTION add COST 1234;
ALTER FUNCTION add COST 1234.5;
ALTER FUNCTION add ROWS 10;
ALTER FUNCTION add ROWS 10.8;

ALTER FUNCTION add SET log_min_messages = ERROR;
ALTER FUNCTION add SET log_min_messages TO DEFAULT;
ALTER FUNCTION add SET log_min_messages FROM CURRENT;
ALTER FUNCTION add RESET log_min_messages;
ALTER FUNCTION add RESET ALL;

-- Create a function with the same name but with a different list of parameters
CREATE FUNCTION add(integer, integer, integer) RETURNS integer
    AS 'select $1 + $2 + $3;'
    LANGUAGE SQL
    IMMUTABLE
    RETURNS NULL ON NULL INPUT;

SELECT * FROM all_functions;

-- Check that the deparsed query contains the correct argument list 
ALTER FUNCTION add(int,int) RESET ALL;

DROP FUNCTION IF EXISTS add(int,int);

-- Check that an invalid function name is still parsed correctly
DROP FUNCTION IF EXISTS missing_function(int, text);
DROP FUNCTION IF EXISTS missing_schema.missing_function(int,float);

SELECT * FROM all_functions;

-- clear objects
SET client_min_messages TO fatal; -- suppress cascading objects dropping
DROP SCHEMA function_tests CASCADE;