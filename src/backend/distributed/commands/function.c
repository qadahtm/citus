/*-------------------------------------------------------------------------
 *
 * function.c
 *    Commands for FUNCTION statements.
 *    The following functions will be supported in Citus:
 *      - this
 *      - that
 *      - TODO: create a list here maybe
 *
 * Copyright (c) 2019, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_proc.h"
#include "distributed/commands.h"
#include "distributed/deparser.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/metadata_sync.h"
#include "distributed/worker_transaction.h"
#include "utils/fmgrprotos.h"
#include "utils/builtins.h"
#include "distributed/metadata/distobject.h"

PG_FUNCTION_INFO_V1(create_distributed_function);


static const char *
GetFunctionDDLCommand(Oid funcOid)
{
	Datum sqlTextDatum = DirectFunctionCall1(pg_get_functiondef, ObjectIdGetDatum(
												 funcOid));
	const char *sql = TextDatumGetCString(sqlTextDatum);
	return sql;
}


Datum
create_distributed_function(PG_FUNCTION_ARGS)
{
	RegProcedure funcOid = PG_GETARG_OID(0);
	const char *ddlCommand = NULL;
	ObjectAddress functionAddress = { 0 };
	ObjectAddressSet(functionAddress, ProcedureRelationId, funcOid);

	EnsureDependenciesExistsOnAllNodes(&functionAddress);

	ddlCommand = GetFunctionDDLCommand(funcOid);

	/* EnsureSequentialModeForTypeDDL(); */
	SendCommandToWorkersAsUser(ALL_WORKERS, DISABLE_DDL_PROPAGATION, NULL);
	SendCommandToWorkersAsUser(ALL_WORKERS, ddlCommand, NULL);

	MarkObjectDistributed(&functionAddress);

	PG_RETURN_VOID();
}


List *
PlanAlterFunctionStmt(AlterFunctionStmt *alterFunctionStmt,
					  const char *querystring)
{
	const char *alterStmtSql = NULL;

	alterStmtSql = deparse_alter_function_stmt(alterFunctionStmt);
	ereport(LOG, (errmsg("deparsed alter function statement"),
				  errdetail("sql: %s", alterStmtSql)));

	return NIL;
}


List *
PlanDropFunctionStmt(DropStmt *dropStmt,
					 const char *queryString)
{
	const char *dropStmtSql = NULL;

	dropStmtSql = deparse_drop_function_stmt(dropStmt);
	ereport(LOG, (errmsg("deparsed drop function statement"),
				  errdetail("sql: %s", dropStmtSql)));

	return NIL;
}
