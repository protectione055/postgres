# database-link Specification (Delta)

## MODIFIED Requirements

### Requirement: Create and manage database links
The system SHALL provide DDL to create, alter, and drop database links.

A `CREATE DATABASE LINK` statement SHALL allow selecting the FDW driver used by the link.

**Syntax (proposed):**
- The FDW driver is selected by providing a reserved key `fdw=<fdw_name>` inside the `USING '<connect_string>'` string.
- Example: `CREATE DATABASE LINK <name> ... USING '... fdw=oracle_fdw'`

If the `USING` connect string does not provide `fdw=<fdw_name>`, the system SHOULD default to `postgres_fdw` (to preserve demo/regression compatibility).

#### Scenario: Create a database link with explicit FDW driver
- **GIVEN** `oracle_fdw` is installed and the user has USAGE privilege on the foreign-data wrapper
- **WHEN** they execute `CREATE DATABASE LINK mylink CONNECT TO remote_user IDENTIFIED BY 'secret' USING 'connstr fdw=oracle_fdw'`
- **THEN** the system stores `oracle_fdw` as the driver for `mylink`
- **AND** creates/updates the corresponding FDW objects using `oracle_fdw`

#### Scenario: Create a database link without FDW clause (default)
- **GIVEN** the environment supports `postgres_fdw`
- **WHEN** the user executes `CREATE DATABASE LINK mylink CONNECT TO remote_user IDENTIFIED BY 'secret' USING 'connstr'`
- **THEN** the system binds `mylink` to `postgres_fdw` by default

## ADDED Requirements

### Requirement: Validate FDW driver selection
The system SHALL validate the selected FDW driver at `CREATE DATABASE LINK` time.

Validation MUST include:
- The FDW exists (installed/visible in `pg_foreign_data_wrapper`).
- The user has required privileges to use the FDW.
- The FDW supports dblink execution and remote metadata discovery required by `object@dblink`.

#### Scenario: FDW does not exist
- **WHEN** a user executes `CREATE DATABASE LINK mylink ... USING 'connstr fdw=no_such_fdw'`
- **THEN** the statement fails with an error indicating the FDW does not exist

#### Scenario: FDW exists but does not support dblink hooks
- **GIVEN** a foreign-data wrapper `some_fdw` exists
- **AND** `some_fdw` does not implement the dblink-required hook(s)
- **WHEN** a user executes `CREATE DATABASE LINK mylink ... USING 'connstr fdw=some_fdw'`
- **THEN** the statement fails with an error indicating the FDW is not supported for `DATABASE LINK`
