/*-------------------------------------------------------------------------
 *
 * pg_dblink.h
 *	  definition of the "database link" system catalog (pg_dblink)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_dblink.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DBLINK_H
#define PG_DBLINK_H

#include "catalog/genbki.h"
#include "catalog/pg_dblink_d.h" /* IWYU pragma: export */

/* ----------------
 *		pg_dblink definition.  cpp turns this into
 *		typedef struct FormData_pg_dblink
 * ----------------
 */
CATALOG(pg_dblink,9382,DbLinkRelationId)
{
	Oid			oid;		/* oid */
	NameData	dblname;		/* database link name */
	Oid			dblowner BKI_LOOKUP(pg_authid);	/* link owner */
	Oid			dblserver BKI_LOOKUP(pg_foreign_server);	/* foreign server */
	Oid			dblrelid BKI_LOOKUP(pg_class);	/* anchor relation */
	char		dblauth;	/* auth mode, see DBLINK_AUTH_* */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		dbloptions[1];	/* link options */
#endif
} FormData_pg_dblink;

/* ----------------
 *		Form_pg_dblink corresponds to a pointer to a tuple with
 *		the format of pg_dblink relation.
 * ----------------
 */
typedef FormData_pg_dblink *Form_pg_dblink;

/* Authentication modes */
#define DBLINK_AUTH_FIXED		'f'
#define DBLINK_AUTH_CURRENT_USER	'c'

DECLARE_TOAST(pg_dblink, 9383, 9384);

DECLARE_UNIQUE_INDEX_PKEY(pg_dblink_oid_index, 9385, DbLinkOidIndexId, pg_dblink, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_dblink_name_index, 9386, DbLinkNameIndexId, pg_dblink, btree(dblname name_ops));

MAKE_SYSCACHE(DBLINKOID, pg_dblink_oid_index, 2);
MAKE_SYSCACHE(DBLINKNAME, pg_dblink_name_index, 2);

#endif						/* PG_DBLINK_H */
