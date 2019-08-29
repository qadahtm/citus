/*
 * local_executor.c
 *
 *  Created on: Aug 28, 2019
 *      Author: onderkalaci
 */

#include "postgres.h"
#include "miscadmin.h"

#include "executor/tuptable.h"
#include "distributed/citus_custom_scan.h"
#include "distributed/multi_executor.h"
#include "distributed/metadata_cache.h"
#include "optimizer/planner.h"
#include "nodes/params.h"
#include "utils/snapmgr.h"
#include "executor/tstoreReceiver.h"

static void CitusLocalExecutor(CitusScanState *node);
static void
ExecuteLocalTaskPlan(CitusScanState *scanState, PlannedStmt *taskPlan, char *queryString);
static void SplitLocalTasks(List *taskList, List **localTaskList, List **remoteTaskList);
static void SplitLocalTaskPlacements(List *taskPlacementList, List **localTaskPlacementList,
						 List **remoteTaskPlacementList);

TupleTableSlot *
LocalExecutorExecScan(CustomScanState *node)
{
	CitusScanState *scanState = (CitusScanState *) node;
	TupleTableSlot *resultSlot = NULL;

	if (!scanState->finishedRemoteScan)
	{
		DistributedPlan *distributedPlan = scanState->distributedPlan;
		Job *workerJob = distributedPlan->workerJob;
		List *taskList = workerJob->taskList;

		TupleDesc tupleDescriptor = ScanStateGetTupleDescriptor(scanState);
		bool randomAccess = true;
		bool interTransactions = false;


		scanState->tuplestorestate =
			tuplestore_begin_heap(randomAccess, interTransactions, work_mem);

		List *localTaskList = NIL;
		List *remoteTaskList = NIL;

		/* we are taking locks on partitions of partitioned tables */
		//LockPartitionsInRelationList(distributedPlan->relationIdList, AccessShareLock);

		SplitLocalTasks(taskList, &localTaskList, &remoteTaskList);

		/* if we didn't have local task, we wouldn't have be here */
		Assert (list_length(localTaskList) > 0);

		workerJob->taskList = localTaskList;
		CitusLocalExecutor((CitusScanState *) node);

		if (list_length(remoteTaskList))
		{
			workerJob->taskList = remoteTaskList;
			AdaptiveExecutor(node, false);
		}

		scanState->finishedRemoteScan = true;
	}

	resultSlot = ReturnTupleFromTuplestore(scanState);

	return resultSlot;
}

/*
 * AdaptiveExecutor is called via CitusExecScan on the
 * first call of CitusExecScan. The function fills the tupleStore
 * of the input scanScate.
 */
static void
CitusLocalExecutor(CitusScanState *node)
{
	DistributedPlan *distributedPlan = ((CitusScanState *) node)->distributedPlan;
	Job *workerJob = distributedPlan->workerJob;
	EState *executorState = ScanStateGetExecutorState(node);
	ParamListInfo paramListInfo = executorState->es_param_list_info;
	List *taskList = workerJob->taskList;
	ListCell *taskCell = NULL;

	foreach(taskCell, taskList)
	{
		Task *task = (Task *) lfirst(taskCell);

		Query *shardQuery = ParseQueryString(task->queryString);
		int cursorOptions = 0;

		PlannedStmt *localPlan =  standard_planner(shardQuery, cursorOptions, paramListInfo);

		ExecuteLocalTaskPlan(node, localPlan, task->queryString);
	}

}

/*
 * ExecuteLocalTaskPlan
 */
static void
ExecuteLocalTaskPlan(CitusScanState *scanState, PlannedStmt *taskPlan, char *queryString)
{
	EState *executorState = ScanStateGetExecutorState(scanState);
	ParamListInfo paramListInfo = executorState->es_param_list_info;
	DestReceiver *dest = CreateDestReceiver(DestTuplestore);
	ScanDirection scanDirection = ForwardScanDirection;
	QueryEnvironment *queryEnv = create_queryEnv();
	QueryDesc *queryDesc = NULL;
	int eflags = 0;

 	SetTuplestoreDestReceiverParams(dest, scanState->tuplestorestate,
									CurrentMemoryContext, false);

 	/* Create a QueryDesc for the query */
	queryDesc = CreateQueryDesc(taskPlan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, paramListInfo, queryEnv, 0);

 	ExecutorStart(queryDesc, eflags);
	ExecutorRun(queryDesc, scanDirection, 0L, true);

 	if (queryDesc->operation != CMD_SELECT)
	{
		/* make sure we get the right completion tag */
		executorState->es_processed = queryDesc->estate->es_processed;
	}

 	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

 	FreeQueryDesc(queryDesc);
}



static void
SplitLocalTasks(List *taskList, List **localTaskList, List **remoteTaskList)
{
	ListCell *taskCell = NULL;

	*remoteTaskList = NIL;
	*localTaskList = NIL;

	foreach(taskCell, taskList)
	{
		Task *task = (Task *) lfirst(taskCell);

		List *localTaskPlacementList = NULL;
		List *remoteTaskPlacementList = NULL;

		SplitLocalTaskPlacements(task->taskPlacementList,
								 &localTaskPlacementList,
								 &remoteTaskPlacementList);

		/* either the local or the remote should be non-nil */
		Assert (!(localTaskPlacementList == NIL && remoteTaskPlacementList == NIL));

		if (localTaskPlacementList == NIL || remoteTaskPlacementList == NIL)
		{
			/* we don't need to split the task */
			if (localTaskPlacementList == NIL)
			{
				*remoteTaskList = lappend(*remoteTaskList, task);
			}
			else
			{
				*localTaskList = lappend(*localTaskList, task);
			}
		}
		else
		{
			Task *localTask = copyObject(task);
			Task *remoteTask = copyObject(task);

			localTask->taskPlacementList = localTaskPlacementList;
			*localTaskList = lappend(*localTaskList, localTask);


			remoteTask->taskPlacementList = remoteTaskPlacementList;
			*remoteTaskList = lappend(*remoteTaskList, remoteTask);

		}

	}
}


static void
SplitLocalTaskPlacements(List *taskPlacementList, List **localTaskPlacementList,
						 List **remoteTaskPlacementList)
{
	ListCell *placementCell = NULL;
	int32 localGroupId = GetLocalGroupId();


	*localTaskPlacementList = NIL;
	*remoteTaskPlacementList = NIL;

	foreach(placementCell, taskPlacementList)
	{
		ShardPlacement *taskPlacement =
			(ShardPlacement *) lfirst(placementCell);

		if (taskPlacement->groupId == localGroupId)
		{
			*localTaskPlacementList = lappend(*localTaskPlacementList, taskPlacement);
		}
		else
		{
			*remoteTaskPlacementList = lappend(*remoteTaskPlacementList, taskPlacement);
		}
	}
}
