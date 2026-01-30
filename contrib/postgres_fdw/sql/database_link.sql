-- ===================================================================
-- tests for database link (@dblink)
-- ===================================================================
-- This test file is intentionally separated from postgres_fdw.sql so the
-- DATABASE LINK feature coverage isn't mixed into the core postgres_fdw
-- regression script.

SELECT current_database() AS current_database,
  current_setting('port') AS current_port
\gset

\set dblink_connstr 'dbname=' :current_database ' port=' :current_port ' meta_ttl=0'
\set dblink_connstr_fdw 'dbname=' :current_database ' port=' :current_port ' fdw=postgres_fdw meta_ttl=0'

CREATE EXTENSION postgres_fdw;

CREATE SCHEMA "dblink_s";
CREATE TABLE "dblink_s"."dblink_t" (
	c1 int NOT NULL,
	CONSTRAINT dblink_t_pkey PRIMARY KEY (c1)
);
INSERT INTO "dblink_s"."dblink_t"
	SELECT id FROM generate_series(1, 1000) id;
ANALYZE "dblink_s"."dblink_t";

-- default FDW selection (should use postgres_fdw)
CREATE DATABASE LINK dblink_default CONNECT TO CURRENT_USER
	USING :'dblink_connstr';
SELECT count(*) FROM "dblink_s"."dblink_t"@dblink_default;
DROP DATABASE LINK dblink_default;

CREATE DATABASE LINK dblink1 CONNECT TO CURRENT_USER
	USING :'dblink_connstr_fdw';

ALTER SERVER dblink1 OPTIONS (
	SET dbname :'current_database',
	SET port :'current_port'
);

SELECT count(*) FROM "dblink_s"."dblink_t"@dblink1;

CREATE VIEW dblink_view AS
	SELECT c1 FROM "dblink_s"."dblink_t"@dblink1 WHERE c1 <= 3;

SELECT count(*) FROM dblink_view;

DROP VIEW dblink_view;
DROP DATABASE LINK dblink1;

-- ===================================================================
-- basic behavior under remote DDL changes
-- ===================================================================
-- Use a dedicated link so DDL-related behavior is isolated.
CREATE DATABASE LINK dblink_ddl CONNECT TO CURRENT_USER
	USING :'dblink_connstr_fdw';

-- baseline query
SELECT count(*) FROM "dblink_s"."dblink_t"@dblink_ddl;
SELECT * FROM "dblink_s"."dblink_t"@dblink_ddl ORDER BY c1 LIMIT 1;

-- create a view over @dblink and verify it survives remote DDL changes
CREATE VIEW dblink_ddl_view AS
	SELECT c1 FROM "dblink_s"."dblink_t"@dblink_ddl WHERE c1 <= 3;
SELECT count(*) FROM dblink_ddl_view;
SELECT * FROM dblink_ddl_view ORDER BY c1;

-- add a column on the remote and query it again through the same link
ALTER TABLE "dblink_s"."dblink_t" ADD COLUMN c2 int;
UPDATE "dblink_s"."dblink_t" SET c2 = c1 * 2;
ANALYZE "dblink_s"."dblink_t";
SELECT min(c2), max(c2) FROM "dblink_s"."dblink_t";
SELECT min(c2), max(c2) FROM "dblink_s"."dblink_t"@dblink_ddl;
SELECT * FROM "dblink_s"."dblink_t"@dblink_ddl ORDER BY c1 LIMIT 1;

-- the view should still work after ADD COLUMN
SELECT count(*) FROM dblink_ddl_view;
SELECT * FROM dblink_ddl_view ORDER BY c1;

-- drop the added column and query again
ALTER TABLE "dblink_s"."dblink_t" DROP COLUMN c2;
SELECT count(*) FROM "dblink_s"."dblink_t"@dblink_ddl;
SELECT * FROM "dblink_s"."dblink_t"@dblink_ddl ORDER BY c1 LIMIT 1;

-- the view should still work after DROP COLUMN
SELECT count(*) FROM dblink_ddl_view;
SELECT * FROM dblink_ddl_view ORDER BY c1;

-- drop and recreate the remote table with a different shape
DROP TABLE "dblink_s"."dblink_t";
CREATE TABLE "dblink_s"."dblink_t" (
	c1 int NOT NULL,
	c3 text,
	CONSTRAINT dblink_t_pkey PRIMARY KEY (c1)
);
INSERT INTO "dblink_s"."dblink_t"
	SELECT id, 'v' || to_char(id, 'FM0000') FROM generate_series(1, 1000) id;
ANALYZE "dblink_s"."dblink_t";

-- query with the same link after recreate
SELECT count(*) FROM "dblink_s"."dblink_t"@dblink_ddl;
SELECT c3 FROM "dblink_s"."dblink_t" ORDER BY c1 LIMIT 1;
SELECT c3 FROM "dblink_s"."dblink_t"@dblink_ddl ORDER BY c1 LIMIT 1;
SELECT * FROM "dblink_s"."dblink_t"@dblink_ddl ORDER BY c1 LIMIT 1;

-- the view should still work after DROP/CREATE
SELECT count(*) FROM dblink_ddl_view;
SELECT * FROM dblink_ddl_view ORDER BY c1;

-- rename the remote table; old name should fail, new name should work
ALTER TABLE "dblink_s"."dblink_t" RENAME TO dblink_t_renamed;
SELECT count(*) FROM "dblink_s"."dblink_t"@dblink_ddl;
SELECT count(*) FROM dblink_ddl_view;
SELECT count(*) FROM "dblink_s"."dblink_t_renamed"@dblink_ddl;

ALTER TABLE "dblink_s"."dblink_t_renamed" RENAME TO dblink_t;

-- the view should work again after rename back
SELECT count(*) FROM dblink_ddl_view;
SELECT * FROM dblink_ddl_view ORDER BY c1;

-- multi-table join over @dblink
CREATE TABLE "dblink_s"."dblink_t2" (
	c1 int NOT NULL,
	c4 text,
	CONSTRAINT dblink_t2_pkey PRIMARY KEY (c1)
);
INSERT INTO "dblink_s"."dblink_t2"
	SELECT id, 'w' || to_char(id, 'FM0000') FROM generate_series(1, 10) id;
ANALYZE "dblink_s"."dblink_t2";
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*)
	FROM "dblink_s"."dblink_t"@dblink_ddl t
	JOIN "dblink_s"."dblink_t2"@dblink_ddl t2
	ON t.c1 = t2.c1;
SELECT count(*)
	FROM "dblink_s"."dblink_t"@dblink_ddl t
	JOIN "dblink_s"."dblink_t2"@dblink_ddl t2
	ON t.c1 = t2.c1;

DROP VIEW dblink_ddl_view;
DROP DATABASE LINK dblink_ddl;

-- USAGE privilege enforcement for selected/default FDW
CREATE ROLE dblink_no_usage;
REVOKE USAGE ON FOREIGN DATA WRAPPER postgres_fdw FROM dblink_no_usage;
SET ROLE dblink_no_usage;
CREATE DATABASE LINK dblink_priv_fail CONNECT TO CURRENT_USER
	USING :'dblink_connstr';
RESET ROLE;
DROP ROLE dblink_no_usage;

DROP SCHEMA "dblink_s" CASCADE;
