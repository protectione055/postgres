/*-------------------------------------------------------------------------
 *
 * foreigncmds.c
 *	  foreign-data wrapper/server creation/manipulation commands
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/foreigncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_dblink.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "tcop/utility.h"
#include "lib/stringinfo.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Helpers for database links
 */
static Oid
get_dblink_oid(const char *name, bool missing_ok)
{
	Oid			result;

	result = GetSysCacheOid1(DBLINKNAME, Anum_pg_dblink_oid,
								  CStringGetDatum(name));

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("database link \"%s\" does not exist", name)));

	return result;
}

/*
 * Parse a libpq-style conninfo string into foreign server options.
 * Only handles key=value pairs with optional SQL-style single quotes.
 */
static List *
connstr_to_server_options(const char *connstr)
{
	List		*options = NIL;
	char		*work;
	char		*p;

	if (connstr == NULL)
		return NIL;

	work = pstrdup(connstr);
	p = work;

	while (*p)
	{
		char		*key_start;
		char		*key_end;
		char		*key;
		char		*val;

		while (isspace((unsigned char) *p))
			p++;
		if (*p == '\0')
			break;

		key_start = p;
		while (*p && *p != '=' && !isspace((unsigned char) *p))
			p++;
		key_end = p;
		while (isspace((unsigned char) *p))
			p++;
		if (*p != '=')
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid connection string syntax")));
		p++; /* skip '=' */

		if (key_end == key_start)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid connection string syntax")));

		key = pnstrdup(key_start, key_end - key_start);

		while (isspace((unsigned char) *p))
			p++;

		if (*p == '\'')
		{
			StringInfoData buf;

			p++; /* skip opening quote */
			initStringInfo(&buf);
			while (*p)
			{
				if (*p == '\'')
				{
					if (*(p + 1) == '\'')
					{
						appendStringInfoChar(&buf, '\'');
						p += 2;
						continue;
					}
					p++; /* closing quote */
					break;
				}
				if (*p == '\\' && *(p + 1) != '\0')
				{
					appendStringInfoChar(&buf, *(p + 1));
					p += 2;
					continue;
				}
				appendStringInfoChar(&buf, *p++);
			}
			if (*(p - 1) != '\'')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid connection string syntax")));
			val = buf.data;
		}
		else
		{
			char	*val_start = p;

			while (*p && !isspace((unsigned char) *p))
				p++;
			val = pnstrdup(val_start, p - val_start);
		}

		/* user/password belong in user mapping, not server options */
		if (strcmp(key, "user") == 0 || strcmp(key, "password") == 0)
			continue;

		/* skip options that postgres_fdw filters out */
		if (strcmp(key, "client_encoding") == 0 ||
			strcmp(key, "fallback_application_name") == 0)
			continue;

		options = lappend(options,
						 makeDefElem(key, (Node *) makeString(pstrdup(val)), -1));
	}

	return options;
}

/*
 * Extract reserved dblink-only parameters from a connection string.
 *
 * Currently supported:
 *   fdw=<fdw_name>
 *
 * The returned *clean_connstr is a normalized key=value string with the
 * reserved parameters removed. The returned *fdwname (if any) is the value
 * of fdw=.
 */
static void
dblink_extract_connstr_params(const char *connstr,
					 char **clean_connstr,
					 char **fdwname,
					 int *meta_ttl)
{
	char		*work;
	char		*p;
	StringInfoData buf;

	*clean_connstr = NULL;
	*fdwname = NULL;
	*meta_ttl = -1;

	if (connstr == NULL)
		return;

	work = pstrdup(connstr);
	p = work;
	initStringInfo(&buf);

	while (*p)
	{
		char		*key_start;
		char		*key_end;
		char		*key;
		char		*val;
		bool		need_quote = false;
		const char	*s;

		while (isspace((unsigned char) *p))
			p++;
		if (*p == '\0')
			break;

		key_start = p;
		while (*p && *p != '=' && !isspace((unsigned char) *p))
			p++;
		key_end = p;
		while (isspace((unsigned char) *p))
			p++;
		if (*p != '=')
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid connection string syntax")));
		p++; /* skip '=' */

		if (key_end == key_start)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("invalid connection string syntax")));

		key = pnstrdup(key_start, key_end - key_start);

		while (isspace((unsigned char) *p))
			p++;

		if (*p == '\'')
		{
			StringInfoData vbuf;

			p++; /* skip opening quote */
			initStringInfo(&vbuf);
			while (*p)
			{
				if (*p == '\'')
				{
					if (*(p + 1) == '\'')
					{
						appendStringInfoChar(&vbuf, '\'');
						p += 2;
						continue;
					}
					p++; /* closing quote */
					break;
				}
				if (*p == '\\' && *(p + 1) != '\0')
				{
					appendStringInfoChar(&vbuf, *(p + 1));
					p += 2;
					continue;
				}
				appendStringInfoChar(&vbuf, *p++);
			}
			if (*(p - 1) != '\'')
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("invalid connection string syntax")));
			val = vbuf.data;
		}
		else
		{
			char	*val_start = p;

			while (*p && !isspace((unsigned char) *p))
				p++;
			val = pnstrdup(val_start, p - val_start);
		}

		/* dblink-reserved parameter: fdw= */
		if (pg_strcasecmp(key, "fdw") == 0)
		{
			if (val[0] == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid fdw value in connection string")));
			if (*fdwname != NULL && pg_strcasecmp(*fdwname, val) != 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple fdw values specified in connection string")));
			if (*fdwname == NULL)
				*fdwname = pstrdup(val);
			continue;
		}

		/* dblink-reserved parameter: meta_ttl= */
		if (pg_strcasecmp(key, "meta_ttl") == 0)
		{
			char *endptr;
			long val_l;

			errno = 0;
			val_l = strtol(val, &endptr, 10);

			if (val[0] == '\0' || *endptr != '\0' || errno == ERANGE || val_l < -1 || val_l > INT_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid meta_ttl value in connection string")));

			if (*meta_ttl != -1 && *meta_ttl != val_l)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("multiple meta_ttl values specified in connection string")));

			*meta_ttl = (int) val_l;
			continue;
		}

		if (buf.len > 0)
			appendStringInfoChar(&buf, ' ');
		appendStringInfo(&buf, "%s=", key);

		if (val[0] == '\0')
			need_quote = true;
		for (s = val; *s; s++)
		{
			if (isspace((unsigned char) *s) || *s == '\'' || *s == '\\')
			{
				need_quote = true;
				break;
			}
		}

		if (!need_quote)
			appendStringInfoString(&buf, val);
		else
		{
			appendStringInfoChar(&buf, '\'');
			for (s = val; *s; s++)
			{
				if (*s == '\'')
					appendStringInfoString(&buf, "''");
				else if (*s == '\\')
					appendStringInfoString(&buf, "\\\\");
				else
					appendStringInfoChar(&buf, *s);
			}
			appendStringInfoChar(&buf, '\'');
		}
	}

	*clean_connstr = buf.data;
}


typedef struct
{
	char	   *tablename;
	char	   *cmd;
} import_error_callback_arg;

/* Internal functions */
static void import_error_callback(void *arg);


/*
 * Convert a DefElem list to the text array format that is used in
 * pg_foreign_data_wrapper, pg_foreign_server, pg_user_mapping, and
 * pg_foreign_table.
 *
 * Returns the array in the form of a Datum, or PointerGetDatum(NULL)
 * if the list is empty.
 *
 * Note: The array is usually stored to database without further
 * processing, hence any validation should be done before this
 * conversion.
 */
static Datum
optionListToArray(List *options)
{
	ArrayBuildState *astate = NULL;
	ListCell   *cell;

	foreach(cell, options)
	{
		DefElem    *def = lfirst(cell);
		const char *name;
		const char *value;
		Size		len;
		text	   *t;

		name = def->defname;
		value = defGetString(def);

		/* Insist that name not contain "=", else "a=b=c" is ambiguous */
		if (strchr(name, '=') != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid option name \"%s\": must not contain \"=\"",
							name)));

		len = VARHDRSZ + strlen(name) + 1 + strlen(value);
		/* +1 leaves room for sprintf's trailing null */
		t = palloc(len + 1);
		SET_VARSIZE(t, len);
		sprintf(VARDATA(t), "%s=%s", name, value);

		astate = accumArrayResult(astate, PointerGetDatum(t),
								  false, TEXTOID,
								  CurrentMemoryContext);
	}

	if (astate)
		return makeArrayResult(astate, CurrentMemoryContext);

	return PointerGetDatum(NULL);
}


/*
 * Transform a list of DefElem into text array format.  This is substantially
 * the same thing as optionListToArray(), except we recognize SET/ADD/DROP
 * actions for modifying an existing list of options, which is passed in
 * Datum form as oldOptions.  Also, if fdwvalidator isn't InvalidOid
 * it specifies a validator function to call on the result.
 *
 * Returns the array in the form of a Datum, or PointerGetDatum(NULL)
 * if the list is empty.
 *
 * This is used by CREATE/ALTER of FOREIGN DATA WRAPPER/SERVER/USER MAPPING/
 * FOREIGN TABLE.
 */
Datum
transformGenericOptions(Oid catalogId,
						Datum oldOptions,
						List *options,
						Oid fdwvalidator)
{
	List	   *resultOptions = untransformRelOptions(oldOptions);
	ListCell   *optcell;
	Datum		result;

	foreach(optcell, options)
	{
		DefElem    *od = lfirst(optcell);
		ListCell   *cell;

		/*
		 * Find the element in resultOptions.  We need this for validation in
		 * all cases.
		 */
		foreach(cell, resultOptions)
		{
			DefElem    *def = lfirst(cell);

			if (strcmp(def->defname, od->defname) == 0)
				break;
		}

		/*
		 * It is possible to perform multiple SET/DROP actions on the same
		 * option.  The standard permits this, as long as the options to be
		 * added are unique.  Note that an unspecified action is taken to be
		 * ADD.
		 */
		switch (od->defaction)
		{
			case DEFELEM_DROP:
				if (!cell)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("option \"%s\" not found",
									od->defname)));
				resultOptions = list_delete_cell(resultOptions, cell);
				break;

			case DEFELEM_SET:
				if (!cell)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("option \"%s\" not found",
									od->defname)));
				lfirst(cell) = od;
				break;

			case DEFELEM_ADD:
			case DEFELEM_UNSPEC:
				if (cell)
					ereport(ERROR,
							(errcode(ERRCODE_DUPLICATE_OBJECT),
							 errmsg("option \"%s\" provided more than once",
									od->defname)));
				resultOptions = lappend(resultOptions, od);
				break;

			default:
				elog(ERROR, "unrecognized action %d on option \"%s\"",
					 (int) od->defaction, od->defname);
				break;
		}
	}

	result = optionListToArray(resultOptions);

	if (OidIsValid(fdwvalidator))
	{
		Datum		valarg = result;

		/*
		 * Pass a null options list as an empty array, so that validators
		 * don't have to be declared non-strict to handle the case.
		 */
		if (DatumGetPointer(valarg) == NULL)
			valarg = PointerGetDatum(construct_empty_array(TEXTOID));
		OidFunctionCall2(fdwvalidator, valarg, ObjectIdGetDatum(catalogId));
	}

	return result;
}


/*
 * Internal workhorse for changing a data wrapper's owner.
 *
 * Allow this only for superusers; also the new owner must be a
 * superuser.
 */
static void
AlterForeignDataWrapperOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_foreign_data_wrapper form;
	Datum		repl_val[Natts_pg_foreign_data_wrapper];
	bool		repl_null[Natts_pg_foreign_data_wrapper];
	bool		repl_repl[Natts_pg_foreign_data_wrapper];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	form = (Form_pg_foreign_data_wrapper) GETSTRUCT(tup);

	/* Must be a superuser to change a FDW owner */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of foreign-data wrapper \"%s\"",
						NameStr(form->fdwname)),
				 errhint("Must be superuser to change owner of a foreign-data wrapper.")));

	/* New owner must also be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of foreign-data wrapper \"%s\"",
						NameStr(form->fdwname)),
				 errhint("The owner of a foreign-data wrapper must be a superuser.")));

	if (form->fdwowner != newOwnerId)
	{
		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_foreign_data_wrapper_fdwowner - 1] = true;
		repl_val[Anum_pg_foreign_data_wrapper_fdwowner - 1] = ObjectIdGetDatum(newOwnerId);

		aclDatum = heap_getattr(tup,
								Anum_pg_foreign_data_wrapper_fdwacl,
								RelationGetDescr(rel),
								&isNull);
		/* Null ACLs do not require changes */
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 form->fdwowner, newOwnerId);
			repl_repl[Anum_pg_foreign_data_wrapper_fdwacl - 1] = true;
			repl_val[Anum_pg_foreign_data_wrapper_fdwacl - 1] = PointerGetDatum(newAcl);
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
								repl_repl);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(ForeignDataWrapperRelationId,
								form->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(ForeignDataWrapperRelationId,
							  form->oid, 0);
}

/*
 * Change foreign-data wrapper owner -- by name
 *
 * Note restrictions in the "_internal" function, above.
 */
ObjectAddress
AlterForeignDataWrapperOwner(const char *name, Oid newOwnerId)
{
	Oid			fdwId;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_foreign_data_wrapper form;


	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNDATAWRAPPERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist", name)));

	form = (Form_pg_foreign_data_wrapper) GETSTRUCT(tup);
	fdwId = form->oid;

	AlterForeignDataWrapperOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, ForeignDataWrapperRelationId, fdwId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change foreign-data wrapper owner -- by OID
 *
 * Note restrictions in the "_internal" function, above.
 */
void
AlterForeignDataWrapperOwner_oid(Oid fwdId, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNDATAWRAPPEROID, ObjectIdGetDatum(fwdId));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper with OID %u does not exist", fwdId)));

	AlterForeignDataWrapperOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Internal workhorse for changing a foreign server's owner
 */
static void
AlterForeignServerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_foreign_server form;
	Datum		repl_val[Natts_pg_foreign_server];
	bool		repl_null[Natts_pg_foreign_server];
	bool		repl_repl[Natts_pg_foreign_server];
	Acl		   *newAcl;
	Datum		aclDatum;
	bool		isNull;

	form = (Form_pg_foreign_server) GETSTRUCT(tup);

	if (form->srvowner != newOwnerId)
	{
		/* Superusers can always do it */
		if (!superuser())
		{
			Oid			srvId;
			AclResult	aclresult;

			srvId = form->oid;

			/* Must be owner */
			if (!object_ownercheck(ForeignServerRelationId, srvId, GetUserId()))
				aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
							   NameStr(form->srvname));

			/* Must be able to become new owner */
			check_can_set_role(GetUserId(), newOwnerId);

			/* New owner must have USAGE privilege on foreign-data wrapper */
			aclresult = object_aclcheck(ForeignDataWrapperRelationId, form->srvfdw, newOwnerId, ACL_USAGE);
			if (aclresult != ACLCHECK_OK)
			{
				ForeignDataWrapper *fdw = GetForeignDataWrapper(form->srvfdw);

				aclcheck_error(aclresult, OBJECT_FDW, fdw->fdwname);
			}
		}

		memset(repl_null, false, sizeof(repl_null));
		memset(repl_repl, false, sizeof(repl_repl));

		repl_repl[Anum_pg_foreign_server_srvowner - 1] = true;
		repl_val[Anum_pg_foreign_server_srvowner - 1] = ObjectIdGetDatum(newOwnerId);

		aclDatum = heap_getattr(tup,
								Anum_pg_foreign_server_srvacl,
								RelationGetDescr(rel),
								&isNull);
		/* Null ACLs do not require changes */
		if (!isNull)
		{
			newAcl = aclnewowner(DatumGetAclP(aclDatum),
								 form->srvowner, newOwnerId);
			repl_repl[Anum_pg_foreign_server_srvacl - 1] = true;
			repl_val[Anum_pg_foreign_server_srvacl - 1] = PointerGetDatum(newAcl);
		}

		tup = heap_modify_tuple(tup, RelationGetDescr(rel), repl_val, repl_null,
								repl_repl);

		CatalogTupleUpdate(rel, &tup->t_self, tup);

		/* Update owner dependency reference */
		changeDependencyOnOwner(ForeignServerRelationId, form->oid,
								newOwnerId);
	}

	InvokeObjectPostAlterHook(ForeignServerRelationId,
							  form->oid, 0);
}

/*
 * Change foreign server owner -- by name
 */
ObjectAddress
AlterForeignServerOwner(const char *name, Oid newOwnerId)
{
	Oid			servOid;
	HeapTuple	tup;
	Relation	rel;
	ObjectAddress address;
	Form_pg_foreign_server form;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNSERVERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", name)));

	form = (Form_pg_foreign_server) GETSTRUCT(tup);
	servOid = form->oid;

	AlterForeignServerOwner_internal(rel, tup, newOwnerId);

	ObjectAddressSet(address, ForeignServerRelationId, servOid);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	return address;
}

/*
 * Change foreign server owner -- by OID
 */
void
AlterForeignServerOwner_oid(Oid srvId, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(FOREIGNSERVEROID, ObjectIdGetDatum(srvId));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign server with OID %u does not exist", srvId)));

	AlterForeignServerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);
}

/*
 * Convert a handler function name passed from the parser to an Oid.
 */
static Oid
lookup_fdw_handler_func(DefElem *handler)
{
	Oid			handlerOid;

	if (handler == NULL || handler->arg == NULL)
		return InvalidOid;

	/* handlers have no arguments */
	handlerOid = LookupFuncName((List *) handler->arg, 0, NULL, false);

	/* check that handler has correct return type */
	if (get_func_rettype(handlerOid) != FDW_HANDLEROID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						NameListToString((List *) handler->arg), "fdw_handler")));

	return handlerOid;
}

/*
 * Convert a validator function name passed from the parser to an Oid.
 */
static Oid
lookup_fdw_validator_func(DefElem *validator)
{
	Oid			funcargtypes[2];

	if (validator == NULL || validator->arg == NULL)
		return InvalidOid;

	/* validators take text[], oid */
	funcargtypes[0] = TEXTARRAYOID;
	funcargtypes[1] = OIDOID;

	return LookupFuncName((List *) validator->arg, 2, funcargtypes, false);
	/* validator's return value is ignored, so we don't check the type */
}

/*
 * Process function options of CREATE/ALTER FDW
 */
static void
parse_func_options(ParseState *pstate, List *func_options,
				   bool *handler_given, Oid *fdwhandler,
				   bool *validator_given, Oid *fdwvalidator)
{
	ListCell   *cell;

	*handler_given = false;
	*validator_given = false;
	/* return InvalidOid if not given */
	*fdwhandler = InvalidOid;
	*fdwvalidator = InvalidOid;

	foreach(cell, func_options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "handler") == 0)
		{
			if (*handler_given)
				errorConflictingDefElem(def, pstate);
			*handler_given = true;
			*fdwhandler = lookup_fdw_handler_func(def);
		}
		else if (strcmp(def->defname, "validator") == 0)
		{
			if (*validator_given)
				errorConflictingDefElem(def, pstate);
			*validator_given = true;
			*fdwvalidator = lookup_fdw_validator_func(def);
		}
		else
			elog(ERROR, "option \"%s\" not recognized",
				 def->defname);
	}
}

/*
 * Create a foreign-data wrapper
 */
ObjectAddress
CreateForeignDataWrapper(ParseState *pstate, CreateFdwStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_foreign_data_wrapper];
	bool		nulls[Natts_pg_foreign_data_wrapper];
	HeapTuple	tuple;
	Oid			fdwId;
	bool		handler_given;
	bool		validator_given;
	Oid			fdwhandler;
	Oid			fdwvalidator;
	Datum		fdwoptions;
	Oid			ownerId;
	ObjectAddress myself;
	ObjectAddress referenced;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create foreign-data wrapper \"%s\"",
						stmt->fdwname),
				 errhint("Must be superuser to create a foreign-data wrapper.")));

	/* For now the owner cannot be specified on create. Use effective user ID. */
	ownerId = GetUserId();

	/*
	 * Check that there is no other foreign-data wrapper by this name.
	 */
	if (GetForeignDataWrapperByName(stmt->fdwname, true) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" already exists",
						stmt->fdwname)));

	/*
	 * Insert tuple into pg_foreign_data_wrapper.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	fdwId = GetNewOidWithIndex(rel, ForeignDataWrapperOidIndexId,
							   Anum_pg_foreign_data_wrapper_oid);
	values[Anum_pg_foreign_data_wrapper_oid - 1] = ObjectIdGetDatum(fdwId);
	values[Anum_pg_foreign_data_wrapper_fdwname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->fdwname));
	values[Anum_pg_foreign_data_wrapper_fdwowner - 1] = ObjectIdGetDatum(ownerId);

	/* Lookup handler and validator functions, if given */
	parse_func_options(pstate, stmt->func_options,
					   &handler_given, &fdwhandler,
					   &validator_given, &fdwvalidator);

	values[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = ObjectIdGetDatum(fdwhandler);
	values[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = ObjectIdGetDatum(fdwvalidator);

	nulls[Anum_pg_foreign_data_wrapper_fdwacl - 1] = true;

	fdwoptions = transformGenericOptions(ForeignDataWrapperRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdwvalidator);

	if (DatumGetPointer(fdwoptions) != NULL)
		values[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = fdwoptions;
	else
		nulls[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* record dependencies */
	myself.classId = ForeignDataWrapperRelationId;
	myself.objectId = fdwId;
	myself.objectSubId = 0;

	if (OidIsValid(fdwhandler))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = fdwhandler;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	if (OidIsValid(fdwvalidator))
	{
		referenced.classId = ProcedureRelationId;
		referenced.objectId = fdwvalidator;
		referenced.objectSubId = 0;
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	recordDependencyOnOwner(ForeignDataWrapperRelationId, fdwId, ownerId);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new foreign data wrapper */
	InvokeObjectPostCreateHook(ForeignDataWrapperRelationId, fdwId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * Alter foreign-data wrapper
 */
ObjectAddress
AlterForeignDataWrapper(ParseState *pstate, AlterFdwStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Form_pg_foreign_data_wrapper fdwForm;
	Datum		repl_val[Natts_pg_foreign_data_wrapper];
	bool		repl_null[Natts_pg_foreign_data_wrapper];
	bool		repl_repl[Natts_pg_foreign_data_wrapper];
	Oid			fdwId;
	bool		isnull;
	Datum		datum;
	bool		handler_given;
	bool		validator_given;
	Oid			fdwhandler;
	Oid			fdwvalidator;
	ObjectAddress myself;

	rel = table_open(ForeignDataWrapperRelationId, RowExclusiveLock);

	/* Must be superuser */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter foreign-data wrapper \"%s\"",
						stmt->fdwname),
				 errhint("Must be superuser to alter a foreign-data wrapper.")));

	tp = SearchSysCacheCopy1(FOREIGNDATAWRAPPERNAME,
							 CStringGetDatum(stmt->fdwname));

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("foreign-data wrapper \"%s\" does not exist", stmt->fdwname)));

	fdwForm = (Form_pg_foreign_data_wrapper) GETSTRUCT(tp);
	fdwId = fdwForm->oid;

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	parse_func_options(pstate, stmt->func_options,
					   &handler_given, &fdwhandler,
					   &validator_given, &fdwvalidator);

	if (handler_given)
	{
		repl_val[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = ObjectIdGetDatum(fdwhandler);
		repl_repl[Anum_pg_foreign_data_wrapper_fdwhandler - 1] = true;

		/*
		 * It could be that the behavior of accessing foreign table changes
		 * with the new handler.  Warn about this.
		 */
		ereport(WARNING,
				(errmsg("changing the foreign-data wrapper handler can change behavior of existing foreign tables")));
	}

	if (validator_given)
	{
		repl_val[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = ObjectIdGetDatum(fdwvalidator);
		repl_repl[Anum_pg_foreign_data_wrapper_fdwvalidator - 1] = true;

		/*
		 * It could be that existing options for the FDW or dependent SERVER,
		 * USER MAPPING or FOREIGN TABLE objects are no longer valid according
		 * to the new validator.  Warn about this.
		 */
		if (OidIsValid(fdwvalidator))
			ereport(WARNING,
					(errmsg("changing the foreign-data wrapper validator can cause "
							"the options for dependent objects to become invalid")));
	}
	else
	{
		/*
		 * Validator is not changed, but we need it for validating options.
		 */
		fdwvalidator = fdwForm->fdwvalidator;
	}

	/*
	 * If options specified, validate and update.
	 */
	if (stmt->options)
	{
		/* Extract the current options */
		datum = SysCacheGetAttr(FOREIGNDATAWRAPPEROID,
								tp,
								Anum_pg_foreign_data_wrapper_fdwoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* Transform the options */
		datum = transformGenericOptions(ForeignDataWrapperRelationId,
										datum,
										stmt->options,
										fdwvalidator);

		if (DatumGetPointer(datum) != NULL)
			repl_val[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = datum;
		else
			repl_null[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;

		repl_repl[Anum_pg_foreign_data_wrapper_fdwoptions - 1] = true;
	}

	/* Everything looks good - update the tuple */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	heap_freetuple(tp);

	ObjectAddressSet(myself, ForeignDataWrapperRelationId, fdwId);

	/* Update function dependencies if we changed them */
	if (handler_given || validator_given)
	{
		ObjectAddress referenced;

		/*
		 * Flush all existing dependency records of this FDW on functions; we
		 * assume there can be none other than the ones we are fixing.
		 */
		deleteDependencyRecordsForClass(ForeignDataWrapperRelationId,
										fdwId,
										ProcedureRelationId,
										DEPENDENCY_NORMAL);

		/* And build new ones. */

		if (OidIsValid(fdwhandler))
		{
			referenced.classId = ProcedureRelationId;
			referenced.objectId = fdwhandler;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}

		if (OidIsValid(fdwvalidator))
		{
			referenced.classId = ProcedureRelationId;
			referenced.objectId = fdwvalidator;
			referenced.objectSubId = 0;
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

	InvokeObjectPostAlterHook(ForeignDataWrapperRelationId, fdwId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * Create a foreign server
 */
ObjectAddress
CreateForeignServer(CreateForeignServerStmt *stmt)
{
	Relation	rel;
	Datum		srvoptions;
	Datum		values[Natts_pg_foreign_server];
	bool		nulls[Natts_pg_foreign_server];
	HeapTuple	tuple;
	Oid			srvId;
	Oid			ownerId;
	AclResult	aclresult;
	ObjectAddress myself;
	ObjectAddress referenced;
	ForeignDataWrapper *fdw;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	/* For now the owner cannot be specified on create. Use effective user ID. */
	ownerId = GetUserId();

	/*
	 * Check that there is no other foreign server by this name.  If there is
	 * one, do nothing if IF NOT EXISTS was specified.
	 */
	srvId = get_foreign_server_oid(stmt->servername, true);
	if (OidIsValid(srvId))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * If we are in an extension script, insist that the pre-existing
			 * object be a member of the extension, to avoid security risks.
			 */
			ObjectAddressSet(myself, ForeignServerRelationId, srvId);
			checkMembershipInCurrentExtension(&myself);

			/* OK to skip */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("server \"%s\" already exists, skipping",
							stmt->servername)));
			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("server \"%s\" already exists",
							stmt->servername)));
	}

	/*
	 * Check that the FDW exists and that we have USAGE on it. Also get the
	 * actual FDW for option validation etc.
	 */
	fdw = GetForeignDataWrapperByName(stmt->fdwname, false);

	aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdw->fdwid, ownerId, ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FDW, fdw->fdwname);

	/*
	 * Insert tuple into pg_foreign_server.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	srvId = GetNewOidWithIndex(rel, ForeignServerOidIndexId,
							   Anum_pg_foreign_server_oid);
	values[Anum_pg_foreign_server_oid - 1] = ObjectIdGetDatum(srvId);
	values[Anum_pg_foreign_server_srvname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->servername));
	values[Anum_pg_foreign_server_srvowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_foreign_server_srvfdw - 1] = ObjectIdGetDatum(fdw->fdwid);

	/* Add server type if supplied */
	if (stmt->servertype)
		values[Anum_pg_foreign_server_srvtype - 1] =
			CStringGetTextDatum(stmt->servertype);
	else
		nulls[Anum_pg_foreign_server_srvtype - 1] = true;

	/* Add server version if supplied */
	if (stmt->version)
		values[Anum_pg_foreign_server_srvversion - 1] =
			CStringGetTextDatum(stmt->version);
	else
		nulls[Anum_pg_foreign_server_srvversion - 1] = true;

	/* Start with a blank acl */
	nulls[Anum_pg_foreign_server_srvacl - 1] = true;

	/* Add server options */
	srvoptions = transformGenericOptions(ForeignServerRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdw->fdwvalidator);

	if (DatumGetPointer(srvoptions) != NULL)
		values[Anum_pg_foreign_server_srvoptions - 1] = srvoptions;
	else
		nulls[Anum_pg_foreign_server_srvoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* record dependencies */
	myself.classId = ForeignServerRelationId;
	myself.objectId = srvId;
	myself.objectSubId = 0;

	referenced.classId = ForeignDataWrapperRelationId;
	referenced.objectId = fdw->fdwid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(ForeignServerRelationId, srvId, ownerId);

	/* dependency on extension */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new foreign server */
	InvokeObjectPostCreateHook(ForeignServerRelationId, srvId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}


/*
 * Alter foreign server
 */
ObjectAddress
AlterForeignServer(AlterForeignServerStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Datum		repl_val[Natts_pg_foreign_server];
	bool		repl_null[Natts_pg_foreign_server];
	bool		repl_repl[Natts_pg_foreign_server];
	Oid			srvId;
	Form_pg_foreign_server srvForm;
	ObjectAddress address;

	rel = table_open(ForeignServerRelationId, RowExclusiveLock);

	tp = SearchSysCacheCopy1(FOREIGNSERVERNAME,
							 CStringGetDatum(stmt->servername));

	if (!HeapTupleIsValid(tp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist", stmt->servername)));

	srvForm = (Form_pg_foreign_server) GETSTRUCT(tp);
	srvId = srvForm->oid;

	/*
	 * Only owner or a superuser can ALTER a SERVER.
	 */
	if (!object_ownercheck(ForeignServerRelationId, srvId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
					   stmt->servername);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (stmt->has_version)
	{
		/*
		 * Change the server VERSION string.
		 */
		if (stmt->version)
			repl_val[Anum_pg_foreign_server_srvversion - 1] =
				CStringGetTextDatum(stmt->version);
		else
			repl_null[Anum_pg_foreign_server_srvversion - 1] = true;

		repl_repl[Anum_pg_foreign_server_srvversion - 1] = true;
	}

	if (stmt->options)
	{
		ForeignDataWrapper *fdw = GetForeignDataWrapper(srvForm->srvfdw);
		Datum		datum;
		bool		isnull;

		/* Extract the current srvoptions */
		datum = SysCacheGetAttr(FOREIGNSERVEROID,
								tp,
								Anum_pg_foreign_server_srvoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* Prepare the options array */
		datum = transformGenericOptions(ForeignServerRelationId,
										datum,
										stmt->options,
										fdw->fdwvalidator);

		if (DatumGetPointer(datum) != NULL)
			repl_val[Anum_pg_foreign_server_srvoptions - 1] = datum;
		else
			repl_null[Anum_pg_foreign_server_srvoptions - 1] = true;

		repl_repl[Anum_pg_foreign_server_srvoptions - 1] = true;
	}

	/* Everything looks good - update the tuple */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	InvokeObjectPostAlterHook(ForeignServerRelationId, srvId, 0);

	ObjectAddressSet(address, ForeignServerRelationId, srvId);

	heap_freetuple(tp);

	table_close(rel, RowExclusiveLock);

	return address;
}


/*
 * Common routine to check permission for user-mapping-related DDL
 * commands.  We allow server owners to operate on any mapping, and
 * users to operate on their own mapping.
 */
static void
user_mapping_ddl_aclcheck(Oid umuserid, Oid serverid, const char *servername)
{
	Oid			curuserid = GetUserId();

	if (!object_ownercheck(ForeignServerRelationId, serverid, curuserid))
	{
		if (umuserid == curuserid)
		{
			AclResult	aclresult;

			aclresult = object_aclcheck(ForeignServerRelationId, serverid, curuserid, ACL_USAGE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, servername);
		}
		else
			aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_SERVER,
						   servername);
	}
}


/*
 * Create user mapping
 */
ObjectAddress
CreateUserMapping(CreateUserMappingStmt *stmt)
{
	Relation	rel;
	Datum		useoptions;
	Datum		values[Natts_pg_user_mapping];
	bool		nulls[Natts_pg_user_mapping];
	HeapTuple	tuple;
	Oid			useId;
	Oid			umId;
	ObjectAddress myself;
	ObjectAddress referenced;
	ForeignServer *srv;
	ForeignDataWrapper *fdw;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	rel = table_open(UserMappingRelationId, RowExclusiveLock);

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
		useId = get_rolespec_oid(stmt->user, false);

	/* Check that the server exists. */
	srv = GetForeignServerByName(stmt->servername, false);

	user_mapping_ddl_aclcheck(useId, srv->serverid, stmt->servername);

	/*
	 * Check that the user mapping is unique within server.
	 */
	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));

	if (OidIsValid(umId))
	{
		if (stmt->if_not_exists)
		{
			/*
			 * Since user mappings aren't members of extensions (see comments
			 * below), no need for checkMembershipInCurrentExtension here.
			 */
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("user mapping for \"%s\" already exists for server \"%s\", skipping",
							MappingUserName(useId),
							stmt->servername)));

			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("user mapping for \"%s\" already exists for server \"%s\"",
							MappingUserName(useId),
							stmt->servername)));
	}

	fdw = GetForeignDataWrapper(srv->fdwid);

	/*
	 * Insert tuple into pg_user_mapping.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	umId = GetNewOidWithIndex(rel, UserMappingOidIndexId,
							  Anum_pg_user_mapping_oid);
	values[Anum_pg_user_mapping_oid - 1] = ObjectIdGetDatum(umId);
	values[Anum_pg_user_mapping_umuser - 1] = ObjectIdGetDatum(useId);
	values[Anum_pg_user_mapping_umserver - 1] = ObjectIdGetDatum(srv->serverid);

	/* Add user options */
	useoptions = transformGenericOptions(UserMappingRelationId,
										 PointerGetDatum(NULL),
										 stmt->options,
										 fdw->fdwvalidator);

	if (DatumGetPointer(useoptions) != NULL)
		values[Anum_pg_user_mapping_umoptions - 1] = useoptions;
	else
		nulls[Anum_pg_user_mapping_umoptions - 1] = true;

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	CatalogTupleInsert(rel, tuple);

	heap_freetuple(tuple);

	/* Add dependency on the server */
	myself.classId = UserMappingRelationId;
	myself.objectId = umId;
	myself.objectSubId = 0;

	referenced.classId = ForeignServerRelationId;
	referenced.objectId = srv->serverid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	if (OidIsValid(useId))
	{
		/* Record the mapped user dependency */
		recordDependencyOnOwner(UserMappingRelationId, umId, useId);
	}

	/*
	 * Perhaps someday there should be a recordDependencyOnCurrentExtension
	 * call here; but since roles aren't members of extensions, it seems like
	 * user mappings shouldn't be either.  Note that the grammar and pg_dump
	 * would need to be extended too if we change this.
	 */

	/* Post creation hook for new user mapping */
	InvokeObjectPostCreateHook(UserMappingRelationId, umId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * Create database link
 */
ObjectAddress
CreateDatabaseLink(CreateDatabaseLinkStmt *stmt)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_dblink];
	bool		nulls[Natts_pg_dblink];
	Oid			dblId;
	Oid			ownerId;
	Oid			srvId;
	Datum		options;
	ObjectAddress	myself;
	ObjectAddress	referenced;
	ObjectAddress	serverAddress;
	ObjectAddress	relAddress;
	CreateForeignServerStmt *srvstmt;
	CreateForeignTableStmt *ftstmt;
	CreateStmt *createstmt;
	CreateUserMappingStmt *umstmt;
	RoleSpec	*connect_role;
	RoleSpec	*mapping_role;
	List		*optlist = NIL;
	List		*srvoptlist = NIL;
	List		*umoptlist = NIL;
	char		authmode = DBLINK_AUTH_CURRENT_USER;
	const char *default_fdwname = "postgres_fdw";
	char		*fdwname = NULL;
	const char *remote_user = NULL;
	const char *anchor_colname = "__dblink_anchor__";
	char		*anchor_relname;
	Oid		anchor_relid = InvalidOid;
	char		*clean_connstr = NULL;
	int			meta_ttl = -1;

	/* Check for duplicate database link name */
	rel = table_open(DbLinkRelationId, RowExclusiveLock);

	dblId = get_dblink_oid(stmt->dblinkname, true);
	if (OidIsValid(dblId))
	{
		if (stmt->if_not_exists)
		{
			ObjectAddressSet(myself, DbLinkRelationId, dblId);
			checkMembershipInCurrentExtension(&myself);

			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("database link \"%s\" already exists, skipping",
							stmt->dblinkname)));
			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("database link \"%s\" already exists",
							stmt->dblinkname)));
	}

	/* Avoid server name conflicts */
	srvId = get_foreign_server_oid(stmt->dblinkname, true);
	if (OidIsValid(srvId))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("server \"%s\" already exists",
						stmt->dblinkname)));

	ownerId = GetUserId();
	connect_role = stmt->username;
	if (connect_role && connect_role->roletype == ROLESPEC_CSTRING)
	{
		authmode = DBLINK_AUTH_FIXED;
		remote_user = connect_role->rolename;
	}

	/*
	 * Extract dblink-reserved parameters from connstr (e.g., fdw=) and build
	 * foreign server options from the cleaned connection string.
	 */
	dblink_extract_connstr_params(stmt->connstr, &clean_connstr, &fdwname, &meta_ttl);
	if (fdwname == NULL)
		fdwname = pstrdup(default_fdwname);

	/* Validate the selected FDW and ensure it supports dblink metadata */
	{
		ForeignDataWrapper *fdw;
		FdwRoutine *fdw_routine;
		AclResult	aclresult;

		fdw = GetForeignDataWrapperByName(fdwname, false);
		aclresult = object_aclcheck(ForeignDataWrapperRelationId, fdw->fdwid,
								ownerId, ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_FDW, fdw->fdwname);

		fdw_routine = GetFdwRoutine(fdw->fdwhandler);
		if (fdw_routine->GetDblinkTableMetadata == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("FDW does not support database link metadata"),
					 errdetail("The foreign-data wrapper \"%s\" lacks database link metadata hook support.",
							   fdwname)));
	}

	srvoptlist = connstr_to_server_options(clean_connstr);

	/* Create foreign server for this link */
	srvstmt = makeNode(CreateForeignServerStmt);
	srvstmt->servername = stmt->dblinkname;
	srvstmt->servertype = NULL;
	srvstmt->version = NULL;
	srvstmt->fdwname = pstrdup(fdwname);
	srvstmt->if_not_exists = false;
	srvstmt->options = srvoptlist;

	serverAddress = CreateForeignServer(srvstmt);
	srvId = serverAddress.objectId;

	/* Make the new server visible to subsequent lookups in this command */
	CommandCounterIncrement();

	/* Create user mapping based on auth mode */
	mapping_role = makeNode(RoleSpec);
	if (authmode == DBLINK_AUTH_FIXED)
	{
		mapping_role->roletype = ROLESPEC_PUBLIC;
		mapping_role->rolename = NULL;
	}
	else
	{
		mapping_role->roletype = ROLESPEC_CURRENT_USER;
		mapping_role->rolename = NULL;
	}
	mapping_role->location = -1;

	if (remote_user)
		umoptlist = lappend(umoptlist,
							makeDefElem("user", (Node *) makeString(pstrdup(remote_user)), -1));
	if (stmt->password)
		umoptlist = lappend(umoptlist,
							makeDefElem("password", (Node *) makeString(pstrdup(stmt->password)), -1));

	umstmt = makeNode(CreateUserMappingStmt);
	umstmt->user = mapping_role;
	umstmt->servername = stmt->dblinkname;
	umstmt->options = umoptlist;
	umstmt->if_not_exists = false;

	(void) CreateUserMapping(umstmt);

	/* Make the new user mapping visible before creating dependent objects */
	CommandCounterIncrement();

	/* Create anchor foreign table for this link */
	{
		RangeVar   *anchor_rv;
		Oid		anchor_nspid;

		anchor_rv = makeRangeVar(NULL, "pg_dblink", -1);
		anchor_nspid = RangeVarGetCreationNamespace(anchor_rv);
		anchor_relname = ChooseRelationName("pg_dblink", stmt->dblinkname,
								"anchor", anchor_nspid, false);

		anchor_rv->relname = anchor_relname;
		anchor_rv->schemaname = get_namespace_name(anchor_nspid);

		createstmt = makeNode(CreateStmt);
		createstmt->relation = anchor_rv;
	}
	createstmt->tableElts = list_make1(makeColumnDef(anchor_colname,
										INT4OID, -1, InvalidOid));
	createstmt->inhRelations = NIL;
	createstmt->partbound = NULL;
	createstmt->partspec = NULL;
	createstmt->ofTypename = NULL;
	createstmt->constraints = NIL;
	createstmt->nnconstraints = NIL;
	createstmt->options = NIL;
	createstmt->oncommit = ONCOMMIT_NOOP;
	createstmt->tablespacename = NULL;
	createstmt->accessMethod = NULL;
	createstmt->if_not_exists = false;

	relAddress = DefineRelation(createstmt, RELKIND_FOREIGN_TABLE, ownerId,
						NULL, NULL);
	anchor_relid = relAddress.objectId;

	ftstmt = makeNode(CreateForeignTableStmt);
	ftstmt->base = *createstmt;
	ftstmt->servername = stmt->dblinkname;
	ftstmt->options = NIL;
	CreateForeignTable(ftstmt, anchor_relid);

	/* Store db link catalog entry */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	dblId = GetNewOidWithIndex(rel, DbLinkOidIndexId,
							  Anum_pg_dblink_oid);
	values[Anum_pg_dblink_oid - 1] = ObjectIdGetDatum(dblId);
	values[Anum_pg_dblink_dblname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->dblinkname));
	values[Anum_pg_dblink_dblowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_dblink_dblserver - 1] = ObjectIdGetDatum(srvId);
	values[Anum_pg_dblink_dblrelid - 1] = ObjectIdGetDatum(anchor_relid);
	values[Anum_pg_dblink_dblauth - 1] = CharGetDatum(authmode);

	if (remote_user)
		optlist = lappend(optlist,
						makeDefElem("user", (Node *) makeString(pstrdup(remote_user)), -1));
	if (stmt->password)
		optlist = lappend(optlist,
						makeDefElem("password", (Node *) makeString(pstrdup(stmt->password)), -1));
	if (clean_connstr)
		optlist = lappend(optlist,
						makeDefElem("connstr", (Node *) makeString(pstrdup(clean_connstr)), -1));
	if (meta_ttl >= 0)
	{
		char		buf[32];

		snprintf(buf, sizeof(buf), "%d", meta_ttl);
		optlist = lappend(optlist,
						makeDefElem("meta_ttl", (Node *) makeString(pstrdup(buf)), -1));
	}

	options = transformGenericOptions(DbLinkRelationId,
							 PointerGetDatum(NULL),
							 optlist,
							 InvalidOid);

	if (DatumGetPointer(options) != NULL)
		values[Anum_pg_dblink_dbloptions - 1] = options;
	else
		nulls[Anum_pg_dblink_dbloptions - 1] = true;

	tup = heap_form_tuple(rel->rd_att, values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	myself.classId = DbLinkRelationId;
	myself.objectId = dblId;
	myself.objectSubId = 0;

	referenced.classId = ForeignServerRelationId;
	referenced.objectId = srvId;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnOwner(DbLinkRelationId, dblId, ownerId);

	if (OidIsValid(anchor_relid))
	{
		ObjectAddress anchorAddress;

		ObjectAddressSet(anchorAddress, RelationRelationId, anchor_relid);
		recordDependencyOn(&anchorAddress, &myself, DEPENDENCY_INTERNAL);
	}

	recordDependencyOnCurrentExtension(&myself, false);

	InvokeObjectPostCreateHook(DbLinkRelationId, dblId, 0);

	table_close(rel, RowExclusiveLock);

	return myself;
}

/*
 * Drop database link
 */
ObjectAddress
DropDatabaseLink(DropDatabaseLinkStmt *stmt)
{
	Relation	rel;
	HeapTuple	tup;
	Form_pg_dblink dblform;
	ObjectAddress address;
	DropUserMappingStmt *umstmt;
	RoleSpec	*mapping_role;
	Oid			srvId;
	Oid			anchorRelid;

	rel = table_open(DbLinkRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(DBLINKNAME, CStringGetDatum(stmt->dblinkname));
	if (!HeapTupleIsValid(tup))
	{
		if (stmt->missing_ok)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("database link \"%s\" does not exist, skipping",
							stmt->dblinkname)));
			table_close(rel, RowExclusiveLock);
			return InvalidObjectAddress;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("database link \"%s\" does not exist",
							stmt->dblinkname)));
	}

	dblform = (Form_pg_dblink) GETSTRUCT(tup);
	address.classId = DbLinkRelationId;
	address.objectId = dblform->oid;
	address.objectSubId = 0;

	if (!object_ownercheck(DbLinkRelationId, dblform->oid, GetUserId()))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be owner of database link \"%s\"",
						stmt->dblinkname)));

	srvId = dblform->dblserver;
	anchorRelid = dblform->dblrelid;

	/* Drop user mapping created for this link */
	mapping_role = makeNode(RoleSpec);
	if (dblform->dblauth == DBLINK_AUTH_FIXED)
	{
		mapping_role->roletype = ROLESPEC_PUBLIC;
		mapping_role->rolename = NULL;
	}
	else
	{
		mapping_role->roletype = ROLESPEC_CURRENT_USER;
		mapping_role->rolename = NULL;
	}
	mapping_role->location = -1;

	umstmt = makeNode(DropUserMappingStmt);
	umstmt->user = mapping_role;
	umstmt->servername = stmt->dblinkname;
	umstmt->missing_ok = true;

	(void) RemoveUserMapping(umstmt);

	CatalogTupleDelete(rel, &tup->t_self);

	/* Remove dependency records for this database link */
	deleteDependencyRecordsFor(DbLinkRelationId, dblform->oid, true);

	InvokeObjectDropHook(DbLinkRelationId, dblform->oid, 0);

	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	if (OidIsValid(anchorRelid))
	{
		ObjectAddress reladdress;

		/* Remove any dependency records between anchor and this link */
		deleteDependencyRecordsForSpecific(RelationRelationId, anchorRelid,
											 DEPENDENCY_INTERNAL,
											 DbLinkRelationId, dblform->oid);
		deleteDependencyRecordsForSpecific(RelationRelationId, anchorRelid,
											 DEPENDENCY_NORMAL,
											 DbLinkRelationId, dblform->oid);
		deleteDependencyRecordsForSpecific(RelationRelationId, anchorRelid,
											 DEPENDENCY_AUTO,
											 DbLinkRelationId, dblform->oid);
		deleteDependencyRecordsForSpecific(RelationRelationId, anchorRelid,
											 DEPENDENCY_EXTENSION,
											 DbLinkRelationId, dblform->oid);

		deleteDependencyRecordsForSpecific(DbLinkRelationId, dblform->oid,
											 DEPENDENCY_INTERNAL,
											 RelationRelationId, anchorRelid);
		deleteDependencyRecordsForSpecific(DbLinkRelationId, dblform->oid,
											 DEPENDENCY_NORMAL,
											 RelationRelationId, anchorRelid);
		deleteDependencyRecordsForSpecific(DbLinkRelationId, dblform->oid,
											 DEPENDENCY_AUTO,
											 RelationRelationId, anchorRelid);
		deleteDependencyRecordsForSpecific(DbLinkRelationId, dblform->oid,
											 DEPENDENCY_EXTENSION,
											 RelationRelationId, anchorRelid);

		/* Make dependency deletions visible before dropping the anchor */
		CommandCounterIncrement();

		ObjectAddressSet(reladdress, RelationRelationId, anchorRelid);
		performDeletion(&reladdress, DROP_RESTRICT, 0);
	}

	/* Drop the foreign server created for this link */
	if (OidIsValid(srvId))
	{
		ObjectAddress srvaddress;

		ObjectAddressSet(srvaddress, ForeignServerRelationId, srvId);
		performDeletion(&srvaddress, DROP_RESTRICT, 0);
	}

	return address;
}


/*
 * Alter user mapping
 */
ObjectAddress
AlterUserMapping(AlterUserMappingStmt *stmt)
{
	Relation	rel;
	HeapTuple	tp;
	Datum		repl_val[Natts_pg_user_mapping];
	bool		repl_null[Natts_pg_user_mapping];
	bool		repl_repl[Natts_pg_user_mapping];
	Oid			useId;
	Oid			umId;
	ForeignServer *srv;
	ObjectAddress address;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	rel = table_open(UserMappingRelationId, RowExclusiveLock);

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
		useId = get_rolespec_oid(stmt->user, false);

	srv = GetForeignServerByName(stmt->servername, false);

	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));
	if (!OidIsValid(umId))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("user mapping for \"%s\" does not exist for server \"%s\"",
						MappingUserName(useId), stmt->servername)));

	user_mapping_ddl_aclcheck(useId, srv->serverid, stmt->servername);

	tp = SearchSysCacheCopy1(USERMAPPINGOID, ObjectIdGetDatum(umId));

	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for user mapping %u", umId);

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));

	if (stmt->options)
	{
		ForeignDataWrapper *fdw;
		Datum		datum;
		bool		isnull;

		/*
		 * Process the options.
		 */

		fdw = GetForeignDataWrapper(srv->fdwid);

		datum = SysCacheGetAttr(USERMAPPINGUSERSERVER,
								tp,
								Anum_pg_user_mapping_umoptions,
								&isnull);
		if (isnull)
			datum = PointerGetDatum(NULL);

		/* Prepare the options array */
		datum = transformGenericOptions(UserMappingRelationId,
										datum,
										stmt->options,
										fdw->fdwvalidator);

		if (DatumGetPointer(datum) != NULL)
			repl_val[Anum_pg_user_mapping_umoptions - 1] = datum;
		else
			repl_null[Anum_pg_user_mapping_umoptions - 1] = true;

		repl_repl[Anum_pg_user_mapping_umoptions - 1] = true;
	}

	/* Everything looks good - update the tuple */
	tp = heap_modify_tuple(tp, RelationGetDescr(rel),
						   repl_val, repl_null, repl_repl);

	CatalogTupleUpdate(rel, &tp->t_self, tp);

	InvokeObjectPostAlterHook(UserMappingRelationId,
							  umId, 0);

	ObjectAddressSet(address, UserMappingRelationId, umId);

	heap_freetuple(tp);

	table_close(rel, RowExclusiveLock);

	return address;
}


/*
 * Drop user mapping
 */
Oid
RemoveUserMapping(DropUserMappingStmt *stmt)
{
	ObjectAddress object;
	Oid			useId;
	Oid			umId;
	ForeignServer *srv;
	RoleSpec   *role = (RoleSpec *) stmt->user;

	if (role->roletype == ROLESPEC_PUBLIC)
		useId = ACL_ID_PUBLIC;
	else
	{
		useId = get_rolespec_oid(stmt->user, stmt->missing_ok);
		if (!OidIsValid(useId))
		{
			/*
			 * IF EXISTS specified, role not found and not public. Notice this
			 * and leave.
			 */
			elog(NOTICE, "role \"%s\" does not exist, skipping",
				 role->rolename);
			return InvalidOid;
		}
	}

	srv = GetForeignServerByName(stmt->servername, true);

	if (!srv)
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("server \"%s\" does not exist",
							stmt->servername)));
		/* IF EXISTS, just note it */
		ereport(NOTICE,
				(errmsg("server \"%s\" does not exist, skipping",
						stmt->servername)));
		return InvalidOid;
	}

	umId = GetSysCacheOid2(USERMAPPINGUSERSERVER, Anum_pg_user_mapping_oid,
						   ObjectIdGetDatum(useId),
						   ObjectIdGetDatum(srv->serverid));

	if (!OidIsValid(umId))
	{
		if (!stmt->missing_ok)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("user mapping for \"%s\" does not exist for server \"%s\"",
							MappingUserName(useId), stmt->servername)));

		/* IF EXISTS specified, just note it */
		ereport(NOTICE,
				(errmsg("user mapping for \"%s\" does not exist for server \"%s\", skipping",
						MappingUserName(useId), stmt->servername)));
		return InvalidOid;
	}

	user_mapping_ddl_aclcheck(useId, srv->serverid, srv->servername);

	/*
	 * Do the deletion
	 */
	object.classId = UserMappingRelationId;
	object.objectId = umId;
	object.objectSubId = 0;

	performDeletion(&object, DROP_CASCADE, 0);

	return umId;
}


/*
 * Create a foreign table
 * call after DefineRelation().
 */
void
CreateForeignTable(CreateForeignTableStmt *stmt, Oid relid)
{
	Relation	ftrel;
	Datum		ftoptions;
	Datum		values[Natts_pg_foreign_table];
	bool		nulls[Natts_pg_foreign_table];
	HeapTuple	tuple;
	AclResult	aclresult;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			ownerId;
	ForeignDataWrapper *fdw;
	ForeignServer *server;

	/*
	 * Advance command counter to ensure the pg_attribute tuple is visible;
	 * the tuple might be updated to add constraints in previous step.
	 */
	CommandCounterIncrement();

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	/*
	 * For now the owner cannot be specified on create. Use effective user ID.
	 */
	ownerId = GetUserId();

	/*
	 * Check that the foreign server exists and that we have USAGE on it. Also
	 * get the actual FDW for option validation etc.
	 */
	server = GetForeignServerByName(stmt->servername, false);
	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid, ownerId, ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	fdw = GetForeignDataWrapper(server->fdwid);

	/*
	 * Insert tuple into pg_foreign_table.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_foreign_table_ftrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_foreign_table_ftserver - 1] = ObjectIdGetDatum(server->serverid);
	/* Add table generic options */
	ftoptions = transformGenericOptions(ForeignTableRelationId,
										PointerGetDatum(NULL),
										stmt->options,
										fdw->fdwvalidator);

	if (DatumGetPointer(ftoptions) != NULL)
		values[Anum_pg_foreign_table_ftoptions - 1] = ftoptions;
	else
		nulls[Anum_pg_foreign_table_ftoptions - 1] = true;

	tuple = heap_form_tuple(ftrel->rd_att, values, nulls);

	CatalogTupleInsert(ftrel, tuple);

	heap_freetuple(tuple);

	/* Add pg_class dependency on the server */
	myself.classId = RelationRelationId;
	myself.objectId = relid;
	myself.objectSubId = 0;

	referenced.classId = ForeignServerRelationId;
	referenced.objectId = server->serverid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	table_close(ftrel, RowExclusiveLock);
}

/*
 * Import a foreign schema
 */
void
ImportForeignSchema(ImportForeignSchemaStmt *stmt)
{
	ForeignServer *server;
	ForeignDataWrapper *fdw;
	FdwRoutine *fdw_routine;
	AclResult	aclresult;
	List	   *cmd_list;
	ListCell   *lc;

	/* Check that the foreign server exists and that we have USAGE on it */
	server = GetForeignServerByName(stmt->server_name, false);
	aclresult = object_aclcheck(ForeignServerRelationId, server->serverid, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FOREIGN_SERVER, server->servername);

	/* Check that the schema exists and we have CREATE permissions on it */
	(void) LookupCreationNamespace(stmt->local_schema);

	/* Get the FDW and check it supports IMPORT */
	fdw = GetForeignDataWrapper(server->fdwid);
	if (!OidIsValid(fdw->fdwhandler))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("foreign-data wrapper \"%s\" has no handler",
						fdw->fdwname)));
	fdw_routine = GetFdwRoutine(fdw->fdwhandler);
	if (fdw_routine->ImportForeignSchema == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_NO_SCHEMAS),
				 errmsg("foreign-data wrapper \"%s\" does not support IMPORT FOREIGN SCHEMA",
						fdw->fdwname)));

	/* Call FDW to get a list of commands */
	cmd_list = fdw_routine->ImportForeignSchema(stmt, server->serverid);

	/* Parse and execute each command */
	foreach(lc, cmd_list)
	{
		char	   *cmd = (char *) lfirst(lc);
		import_error_callback_arg callback_arg;
		ErrorContextCallback sqlerrcontext;
		List	   *raw_parsetree_list;
		ListCell   *lc2;

		/*
		 * Setup error traceback support for ereport().  This is so that any
		 * error in the generated SQL will be displayed nicely.
		 */
		callback_arg.tablename = NULL;	/* not known yet */
		callback_arg.cmd = cmd;
		sqlerrcontext.callback = import_error_callback;
		sqlerrcontext.arg = &callback_arg;
		sqlerrcontext.previous = error_context_stack;
		error_context_stack = &sqlerrcontext;

		/*
		 * Parse the SQL string into a list of raw parse trees.
		 */
		raw_parsetree_list = pg_parse_query(cmd);

		/*
		 * Process each parse tree (we allow the FDW to put more than one
		 * command per string, though this isn't really advised).
		 */
		foreach(lc2, raw_parsetree_list)
		{
			RawStmt    *rs = lfirst_node(RawStmt, lc2);
			CreateForeignTableStmt *cstmt = (CreateForeignTableStmt *) rs->stmt;
			PlannedStmt *pstmt;

			/*
			 * Because we only allow CreateForeignTableStmt, we can skip parse
			 * analysis, rewrite, and planning steps here.
			 */
			if (!IsA(cstmt, CreateForeignTableStmt))
				elog(ERROR,
					 "foreign-data wrapper \"%s\" returned incorrect statement type %d",
					 fdw->fdwname, (int) nodeTag(cstmt));

			/* Ignore commands for tables excluded by filter options */
			if (!IsImportableForeignTable(cstmt->base.relation->relname, stmt))
				continue;

			/* Enable reporting of current table's name on error */
			callback_arg.tablename = cstmt->base.relation->relname;

			/* Ensure creation schema is the one given in IMPORT statement */
			cstmt->base.relation->schemaname = pstrdup(stmt->local_schema);

			/* No planning needed, just make a wrapper PlannedStmt */
			pstmt = makeNode(PlannedStmt);
			pstmt->commandType = CMD_UTILITY;
			pstmt->canSetTag = false;
			pstmt->utilityStmt = (Node *) cstmt;
			pstmt->stmt_location = rs->stmt_location;
			pstmt->stmt_len = rs->stmt_len;
			pstmt->planOrigin = PLAN_STMT_INTERNAL;

			/* Execute statement */
			ProcessUtility(pstmt, cmd, false,
						   PROCESS_UTILITY_SUBCOMMAND, NULL, NULL,
						   None_Receiver, NULL);

			/* Be sure to advance the command counter between subcommands */
			CommandCounterIncrement();

			callback_arg.tablename = NULL;
		}

		error_context_stack = sqlerrcontext.previous;
	}
}

/*
 * error context callback to let us supply the failing SQL statement's text
 */
static void
import_error_callback(void *arg)
{
	import_error_callback_arg *callback_arg = (import_error_callback_arg *) arg;
	int			syntaxerrposition;

	/* If it's a syntax error, convert to internal syntax error report */
	syntaxerrposition = geterrposition();
	if (syntaxerrposition > 0)
	{
		errposition(0);
		internalerrposition(syntaxerrposition);
		internalerrquery(callback_arg->cmd);
	}

	if (callback_arg->tablename)
		errcontext("importing foreign table \"%s\"",
				   callback_arg->tablename);
}
