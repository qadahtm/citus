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
extern int MultiShardConnectionType;


extern bool WritableStandbyCoordinator;
extern bool ForceMaxQueryParallelization;
extern int MaxAdaptiveExecutorPoolSize;
extern int ExecutorSlowStartInterval;


extern void CitusExecutorStart(QueryDesc *queryDesc, int eflags);
extern void CitusExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
							 bool execute_once);
extern TupleTableSlot * AdaptiveExecutor(CustomScanState *node);
extern uint64 ExecuteTaskListExtended(RowModifyLevel modLevel, List *taskList,
									  TupleDesc tupleDescriptor,
									  Tuplestorestate *tupleStore,
									  bool hasReturning, int targetPoolSize);
extern void ExecuteUtilityTaskListWithoutResults(List *taskList);
extern uint64 ExecuteTaskList(RowModifyLevel modLevel, List *taskList, int
							  targetPoolSize);
extern TupleTableSlot * CitusExecScan(CustomScanState *node);
extern TupleTableSlot * ReturnTupleFromTuplestore(CitusScanState *scanState);
extern void LoadTuplesIntoTupleStore(CitusScanState *citusScanState, Job *workerJob);
extern void ReadFileIntoTupleStore(char *fileName, char *copyFormat, TupleDesc
								   tupleDescriptor, Tuplestorestate *tupstore);
extern Query * ParseQueryString(const char *queryString, Oid *paramOids, int numParams);
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
extern bool TaskListModifiesDatabase(RowModifyLevel modLevel, List *taskList);


#endif /* MULTI_EXECUTOR_H */
