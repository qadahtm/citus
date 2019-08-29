/*-------------------------------------------------------------------------
 *
 * multi_executor.h
 *	  Executor support for Citus.
 *
 * Copyright (c) 2012-2016, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#ifndef MULTI_EXECUTOR_H
#define MULTI_EXECUTOR_H

#include "executor/execdesc.h"
#include "nodes/parsenodes.h"
#include "nodes/execnodes.h"

#include "distributed/citus_custom_scan.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_server_executor.h"


/* managed via guc.c */
typedef enum
{
	PARALLEL_CONNECTION = 0,
	SEQUENTIAL_CONNECTION = 1
} MultiShardConnectionTypes;


/*
 * ExecutionLevel describes the level exectuion for the distributed query execution.
 * In Citus MX, where the data nodes also hold the metadata, it is possible to skip
 * connection establishment and execute the query locally within the same local
 * transaction.
 *
 * This enum keeps track of the levels of execution happening within the
 * transaction blocks. Note that use this enum even if we're not in a
 * transaction block, but that's only to be consistent.
 */
typedef enum ExecutionLevel
{
	/* nothing has been executed yet */
	EXECUTION_LEVEL_NONE,

	/*
	 * Only executions on the shards that are local to the current
	 * node has happened within the current transaction. So, no
	 * connections has been used at all.
	 */
	EXECUTION_LEVEL_LOCAL_ONLY,

	/*
	 *
	 */
	EXECUTION_LEVEL_INCLUDING_LOCAL,

	/*
	 *
	 */
	EXECUTION_LEVEL_REMOTE_ONLY

} ExecutionLevel;




extern int MultiShardConnectionType;


extern bool WritableStandbyCoordinator;
extern bool ForceMaxQueryParallelization;
extern int MaxAdaptiveExecutorPoolSize;
extern int ExecutorSlowStartInterval;


extern void CitusExecutorStart(QueryDesc *queryDesc, int eflags);
extern void CitusExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
							 bool execute_once);
extern TupleTableSlot * AdaptiveExecutor(CustomScanState *node, bool startTupleStore);
extern uint64 ExecuteTaskListExtended(RowModifyLevel modLevel, List *taskList,
									  TupleDesc tupleDescriptor,
									  Tuplestorestate *tupleStore,
									  bool hasReturning, int targetPoolSize);
extern void ExecuteUtilityTaskListWithoutResults(List *taskList);
extern uint64 ExecuteTaskList(RowModifyLevel modLevel, List *taskList, int
							  targetPoolSize);
extern TupleTableSlot * CitusExecScan(CustomScanState *node);
TupleTableSlot *
LocalExecutorExecScan(CustomScanState *node);
extern TupleTableSlot * ReturnTupleFromTuplestore(CitusScanState *scanState);
extern void LoadTuplesIntoTupleStore(CitusScanState *citusScanState, Job *workerJob);
extern void ReadFileIntoTupleStore(char *fileName, char *copyFormat, TupleDesc
								   tupleDescriptor, Tuplestorestate *tupstore);
extern Query * ParseQueryString(const char *queryString);
extern void ExecuteQueryStringIntoDestReceiver(const char *queryString, ParamListInfo
											   params,
											   DestReceiver *dest);
extern void ExecuteQueryIntoDestReceiver(Query *query, ParamListInfo params,
										 DestReceiver *dest);
extern void ExecutePlanIntoDestReceiver(PlannedStmt *queryPlan, ParamListInfo params,
										DestReceiver *dest);
extern void SetLocalMultiShardModifyModeToSequential(void);
extern void SetLocalForceMaxQueryParallelization(void);
extern void SortTupleStore(CitusScanState *scanState);


#endif /* MULTI_EXECUTOR_H */
