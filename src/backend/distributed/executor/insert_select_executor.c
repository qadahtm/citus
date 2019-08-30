/*-------------------------------------------------------------------------
 *
 * insert_select_executor.c
 *
 * Executor logic for INSERT..SELECT.
 *
 * Copyright (c) 2017, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "distributed/citus_ruleutils.h"
#include "distributed/colocation_utils.h"
#include "distributed/commands/multi_copy.h"
#include "distributed/insert_select_executor.h"
#include "distributed/insert_select_planner.h"
#include "distributed/multi_executor.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/multi_router_executor.h"
#include "distributed/multi_router_planner.h"
#include "distributed/distributed_planner.h"
#include "distributed/recursive_planning.h"
#include "distributed/redistribution.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_management.h"
#include "executor/executor.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "utils/lsyscache.h"
#include "utils/portal.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"


static Query * WrapSubquery(Query *subquery);
static bool IsRedistributablePlan(PlannedStmt *selectPlan);
static List * RedistributedInsertSelectTaskList(Query *insertSelectQuery,
												RedistributedQueryResult *redistributionResult);
static void AddInsertSelectCasts(List *targetList, TupleDesc destTupleDescriptor);
static List * TwoPhaseInsertSelectTaskList(Query *insertSelectQuery, char *resultIdPrefix);
static void ExecutePlanIntoRelation(Oid targetRelationId, List *insertTargetList,
									PlannedStmt *selectPlan, EState *executorState);
static HTAB * ExecutePlanIntoColocatedIntermediateResults(Oid targetRelationId,
														  List *insertTargetList,
														  PlannedStmt *selectPlan,
														  EState *executorState,
														  char *intermediateResultIdPrefix);
static List * BuildColumnNameListFromTargetList(Oid targetRelationId,
												List *insertTargetList);
static int PartitionColumnIndexFromColumnList(Oid relationId, List *columnNameList);


/*
 * CoordinatorInsertSelectExecScan executes an INSERT INTO distributed_table
 * SELECT .. query by setting up a DestReceiver that copies tuples into the
 * distributed table and then executing the SELECT query using that DestReceiver
 * as the tuple destination.
 */
TupleTableSlot *
CoordinatorInsertSelectExecScan(CustomScanState *node)
{
	CitusScanState *scanState = (CitusScanState *) node;
	TupleTableSlot *resultSlot = NULL;

	if (!scanState->finishedRemoteScan)
	{
		EState *executorState = scanState->customScanState.ss.ps.state;
		ParamListInfo paramListInfo = executorState->es_param_list_info;
		DistributedPlan *distributedPlan = scanState->distributedPlan;
		Query *insertSelectQuery = copyObject(distributedPlan->insertSelectQuery);
		Query *selectQuery = NULL;
		List *insertTargetList = insertSelectQuery->targetList;
		RangeTblEntry *selectRte = ExtractSelectRangeTableEntry(insertSelectQuery);
		RangeTblEntry *insertRte = ExtractResultRelationRTE(insertSelectQuery);
		Oid targetRelationId = insertRte->relid;
		char *intermediateResultIdPrefix = distributedPlan->intermediateResultIdPrefix;
		bool hasReturning = distributedPlan->hasReturning;
		int cursorOptions = 0;
		PlannedStmt *selectPlan = NULL;
		HTAB *shardStateHash = NULL;

		ereport(DEBUG1, (errmsg("Collecting INSERT ... SELECT results on coordinator")));

		/* move CTEs from INSERT...SELECT to SELECT */
		selectQuery = BuildSelectForInsertSelect(insertSelectQuery);
		selectRte->subquery = selectQuery;

		ReorderInsertSelectTargetLists(insertSelectQuery, insertRte, selectRte);

		/*
		 * Make a copy of the query, since pg_plan_query may scribble on it and we
		 * want it to be replanned every time if it is stored in a prepared
		 * statement.
		 */
		selectQuery = copyObject(selectQuery);

		/* plan the subquery, this may be another distributed query */
		cursorOptions = CURSOR_OPT_PARALLEL_OK;
		selectPlan = pg_plan_query(selectQuery, cursorOptions, paramListInfo);

		/*
		 * If we are dealing with partitioned table, we also need to lock its
		 * partitions. Here we only lock targetRelation, we acquire necessary
		 * locks on selected tables during execution of those select queries.
		 */
		if (PartitionedTable(targetRelationId))
		{
			LockPartitionRelations(targetRelationId, RowExclusiveLock);
		}

		if (IsRedistributablePlan(selectPlan))
		{
			DistributedPlan *distSelectPlan =
				GetDistributedPlan((CustomScan *) selectPlan->planTree);
			RedistributedQueryResult *result = NULL;
			char *distResultPrefix = NULL;
			List *columnNameList = NIL;
			int distributionColumnIndex = 0;
			DistributionScheme *targetDistribution = NULL;
			bool isForWrites = true;
			List *taskList = NIL;
			TupleDesc tupleDescriptor = ScanStateGetTupleDescriptor(scanState);
			bool randomAccess = true;
			bool interTransactions = false;
			int64 rowsInserted = 0;

			distResultPrefix = "replace_this";

			columnNameList = BuildColumnNameListFromTargetList(targetRelationId,
															   insertTargetList);
			distributionColumnIndex = PartitionColumnIndexFromColumnList(targetRelationId,
																		  columnNameList);


			targetDistribution = GetDistributionSchemeForRelationId(targetRelationId);

			result = RedistributeDistributedPlanResult(distResultPrefix,
													   distSelectPlan,
													   distributionColumnIndex,
													   targetDistribution,
													   isForWrites);

			taskList = RedistributedInsertSelectTaskList(insertSelectQuery, result);

			scanState->tuplestorestate =
				tuplestore_begin_heap(randomAccess, interTransactions, work_mem);

			rowsInserted = ExecuteTaskListExtended(ROW_MODIFY_COMMUTATIVE, taskList,
												   tupleDescriptor,
												   scanState->tuplestorestate,
												   hasReturning,
												   MaxAdaptiveExecutorPoolSize);		

			executorState->es_processed = rowsInserted;
		}
		else if (insertSelectQuery->onConflict || hasReturning)
		{

			/*
			 * If we also have a workerJob that means there is a second step
			 * to the INSERT...SELECT. This happens when there is a RETURNING
			 * or ON CONFLICT clause which is implemented as a separate
			 * distributed INSERT...SELECT from a set of intermediate results
			 * to the target relation.
			 */
			ListCell *taskCell = NULL;
			List *taskList = NIL;
			List *prunedTaskList = NIL;

			shardStateHash = ExecutePlanIntoColocatedIntermediateResults(
				targetRelationId,
				insertTargetList,
				selectPlan,
				executorState,
				intermediateResultIdPrefix);

			/* generate tasks for the INSERT..SELECT phase */
			taskList = TwoPhaseInsertSelectTaskList(insertSelectQuery,
													intermediateResultIdPrefix);

			/*
			 * We cannot actually execute INSERT...SELECT tasks that read from
			 * intermediate results that weren't created because no rows were
			 * written to them. Prune those tasks out by only including tasks
			 * on shards with connections.
			 */
			foreach(taskCell, taskList)
			{
				Task *task = (Task *) lfirst(taskCell);
				uint64 shardId = task->anchorShardId;
				bool shardModified = false;

				hash_search(shardStateHash, &shardId, HASH_FIND, &shardModified);
				if (shardModified)
				{
					prunedTaskList = lappend(prunedTaskList, task);
				}
			}

			if (prunedTaskList != NIL)
			{
				if (TaskExecutorType != MULTI_EXECUTOR_ADAPTIVE)
				{
					if (MultiShardConnectionType == SEQUENTIAL_CONNECTION)
					{
						ExecuteModifyTasksSequentially(scanState, prunedTaskList,
													   ROW_MODIFY_COMMUTATIVE,
													   hasReturning);
					}
					else
					{
						ExecuteMultipleTasks(scanState, prunedTaskList, true,
											 hasReturning);
					}
				}
				else
				{
					TupleDesc tupleDescriptor = ScanStateGetTupleDescriptor(scanState);
					bool randomAccess = true;
					bool interTransactions = false;

					Assert(scanState->tuplestorestate == NULL);
					scanState->tuplestorestate =
						tuplestore_begin_heap(randomAccess, interTransactions, work_mem);

					ExecuteTaskListExtended(ROW_MODIFY_COMMUTATIVE, prunedTaskList,
											tupleDescriptor, scanState->tuplestorestate,
											hasReturning, MaxAdaptiveExecutorPoolSize);
				}

				if (SortReturning && hasReturning)
				{
					SortTupleStore(scanState);
				}
			}
		}
		else
		{
			ExecutePlanIntoRelation(targetRelationId, insertTargetList, selectPlan,
									executorState);
		}

		scanState->finishedRemoteScan = true;
	}

	resultSlot = ReturnTupleFromTuplestore(scanState);

	return resultSlot;
}


/*
 * BuildSelectForInsertSelect extracts the SELECT part from an INSERT...SELECT query.
 * If the INSERT...SELECT has CTEs then these are added to the resulting SELECT instead.
 */
Query *
BuildSelectForInsertSelect(Query *insertSelectQuery)
{
	RangeTblEntry *selectRte = ExtractSelectRangeTableEntry(insertSelectQuery);
	Query *selectQuery = selectRte->subquery;

	/*
	 * Wrap the SELECT as a subquery if the INSERT...SELECT has CTEs or the SELECT
	 * has top-level set operations.
	 *
	 * We could simply wrap all queries, but that might create a subquery that is
	 * not supported by the logical planner. Since the logical planner also does
	 * not support CTEs and top-level set operations, we can wrap queries containing
	 * those without breaking anything.
	 */
	if (list_length(insertSelectQuery->cteList) > 0)
	{
		selectQuery = WrapSubquery(selectRte->subquery);

		/* copy CTEs from the INSERT ... SELECT statement into outer SELECT */
		selectQuery->cteList = copyObject(insertSelectQuery->cteList);
		selectQuery->hasModifyingCTE = insertSelectQuery->hasModifyingCTE;
	}
	else if (selectQuery->setOperations != NULL)
	{
		/* top-level set operations confuse the ReorderInsertSelectTargetLists logic */
		selectQuery = WrapSubquery(selectRte->subquery);
	}

	return selectQuery;
}


/*
 * WrapSubquery wraps the given query as a subquery in a newly constructed
 * "SELECT * FROM (...subquery...) citus_insert_select_subquery" query.
 */
static Query *
WrapSubquery(Query *subquery)
{
	Query *outerQuery = NULL;
	ParseState *pstate = make_parsestate(NULL);
	Alias *selectAlias = NULL;
	RangeTblEntry *newRangeTableEntry = NULL;
	RangeTblRef *newRangeTableRef = NULL;
	ListCell *selectTargetCell = NULL;
	List *newTargetList = NIL;

	outerQuery = makeNode(Query);
	outerQuery->commandType = CMD_SELECT;

	/* create range table entries */
	selectAlias = makeAlias("citus_insert_select_subquery", NIL);
	newRangeTableEntry = addRangeTableEntryForSubquery(pstate, subquery,
													   selectAlias, false, true);
	outerQuery->rtable = list_make1(newRangeTableEntry);

	/* set the FROM expression to the subquery */
	newRangeTableRef = makeNode(RangeTblRef);
	newRangeTableRef->rtindex = 1;
	outerQuery->jointree = makeFromExpr(list_make1(newRangeTableRef), NULL);

	/* create a target list that matches the SELECT */
	foreach(selectTargetCell, subquery->targetList)
	{
		TargetEntry *selectTargetEntry = (TargetEntry *) lfirst(selectTargetCell);
		Var *newSelectVar = NULL;
		TargetEntry *newSelectTargetEntry = NULL;

		/* exactly 1 entry in FROM */
		int indexInRangeTable = 1;

		if (selectTargetEntry->resjunk)
		{
			continue;
		}

		newSelectVar = makeVar(indexInRangeTable, selectTargetEntry->resno,
							   exprType((Node *) selectTargetEntry->expr),
							   exprTypmod((Node *) selectTargetEntry->expr),
							   exprCollation((Node *) selectTargetEntry->expr), 0);

		newSelectTargetEntry = makeTargetEntry((Expr *) newSelectVar,
											   selectTargetEntry->resno,
											   selectTargetEntry->resname,
											   selectTargetEntry->resjunk);

		newTargetList = lappend(newTargetList, newSelectTargetEntry);
	}

	outerQuery->targetList = newTargetList;

	return outerQuery;
}


static bool
IsRedistributablePlan(PlannedStmt *selectPlan)
{
	Plan *planTree = selectPlan->planTree;
	DistributedPlan *distributedPlan = NULL;

	if (!IsA(planTree, CustomScan))
	{
		return false;
	}

	distributedPlan = GetDistributedPlan((CustomScan *) planTree);
	if (distributedPlan == NULL)
	{
		return false;
	}

	/* TODO: additional checks */

	return true;
}


/*
 *
 */
static List *
RedistributedInsertSelectTaskList(Query *insertSelectQuery,
								  RedistributedQueryResult *redistributionResult)
{
	List *taskList = NIL;

	/*
	 * Make a copy of the INSERT ... SELECT. We'll repeatedly replace the
	 * subquery of insertResultQuery for different intermediate results and
	 * then deparse it.
	 */
	Query *insertResultQuery = copyObject(insertSelectQuery);
	RangeTblEntry *insertRte = ExtractResultRelationRTE(insertResultQuery);
	RangeTblEntry *selectRte = ExtractSelectRangeTableEntry(insertResultQuery);
	Oid targetRelationId = insertRte->relid;
	DistTableCacheEntry *targetCacheEntry = DistributedTableCacheEntry(targetRelationId);

	int shardCount = targetCacheEntry->shardIntervalArrayLength;
	int shardOffset = 0;
	uint32 taskIdIndex = 1;
	uint64 jobId = INVALID_JOB_ID;
	char *resultIdPrefix = redistributionResult->resultPrefix;

	Relation distributedRelation = NULL;
	TupleDesc destTupleDescriptor = NULL;

	distributedRelation = heap_open(targetRelationId, RowExclusiveLock);
	destTupleDescriptor = RelationGetDescr(distributedRelation);

	/*
	 * If the type of insert column and target table's column type is
	 * different from each other. Cast insert column't type to target
	 * table's column
	 */
	AddInsertSelectCasts(insertSelectQuery->targetList, destTupleDescriptor);

	for (shardOffset = 0; shardOffset < shardCount; shardOffset++)
	{
		ShardInterval *targetShardInterval =
			targetCacheEntry->sortedShardIntervalArray[shardOffset];
		ReassembledFragmentSet *fragmentSet =
			&(redistributionResult->reassembledFragmentSets[shardOffset]);
		uint64 shardId = targetShardInterval->shardId;
		List *insertShardPlacementList = NIL;
		Query *fragmentSetQuery = NULL;
		StringInfo queryString = makeStringInfo();
		RelationShard *relationShard = NULL;
		Task *modifyTask = NULL;

		/* generate the query on the intermediate result */
		fragmentSetQuery = ReadReassembledFragmentSetQuery(resultIdPrefix,
														   insertSelectQuery->targetList,
														   fragmentSet);

		/* put the intermediate result query in the INSERT..SELECT */
		selectRte->subquery = fragmentSetQuery;

		/* setting an alias simplifies deparsing of RETURNING */
		if (insertRte->alias == NULL)
		{
			Alias *alias = makeAlias(CITUS_TABLE_ALIAS, NIL);
			insertRte->alias = alias;
		}

		/*
		 * Generate a query string for the query that inserts into a shard and reads
		 * from an intermediate result.
		 *
		 * Since CTEs have already been converted to intermediate results, they need
		 * to removed from the query. Otherwise, worker queries include both
		 * intermediate results and CTEs in the query.
		 */
		insertResultQuery->cteList = NIL;
		deparse_shard_query(insertResultQuery, targetRelationId, shardId, queryString);
		ereport(DEBUG2, (errmsg("distributed statement: %s", queryString->data)));

		LockShardDistributionMetadata(shardId, ShareLock);
		insertShardPlacementList = FinalizedShardPlacementList(shardId);

		relationShard = CitusMakeNode(RelationShard);
		relationShard->relationId = targetShardInterval->relationId;
		relationShard->shardId = targetShardInterval->shardId;

		modifyTask = CreateBasicTask(jobId, taskIdIndex, MODIFY_TASK, queryString->data);
		modifyTask->dependedTaskList = NULL;
		modifyTask->anchorShardId = shardId;
		modifyTask->taskPlacementList = insertShardPlacementList;
		modifyTask->relationShardList = list_make1(relationShard);
		modifyTask->replicationModel = targetCacheEntry->replicationModel;

		taskList = lappend(taskList, modifyTask);

		taskIdIndex++;
	}

	heap_close(distributedRelation, NoLock);

	return taskList;
}


static void
AddInsertSelectCasts(List *targetList, TupleDesc destTupleDescriptor)
{
	ListCell *targetEntryCell = NULL;

	foreach(targetEntryCell, targetList)
	{
		TargetEntry *targetEntry = (TargetEntry *) lfirst(targetEntryCell);
		Var *insertColumn = (Var *) targetEntry->expr;
		Form_pg_attribute attr = TupleDescAttr(destTupleDescriptor, targetEntry->resno -
											   1);

		if (insertColumn->vartype != attr->atttypid)
		{
			CoerceViaIO *coerceExpr = makeNode(CoerceViaIO);
			coerceExpr->arg = (Expr *) copyObject(insertColumn);
			coerceExpr->resulttype = attr->atttypid;
			coerceExpr->resultcollid = attr->attcollation;
			coerceExpr->coerceformat = COERCE_IMPLICIT_CAST;
			coerceExpr->location = -1;

			targetEntry->expr = (Expr *) coerceExpr;
		}
	}
}


/*
 * TwoPhaseInsertSelectTaskList generates a list of tasks for a query that
 * inserts into a target relation and selects from a set of co-located
 * intermediate results.
 */
static List *
TwoPhaseInsertSelectTaskList(Query *insertSelectQuery, char *resultIdPrefix)
{
	List *taskList = NIL;

	/*
	 * Make a copy of the INSERT ... SELECT. We'll repeatedly replace the
	 * subquery of insertResultQuery for different intermediate results and
	 * then deparse it.
	 */
	Query *insertResultQuery = copyObject(insertSelectQuery);
	RangeTblEntry *insertRte = ExtractResultRelationRTE(insertResultQuery);
	RangeTblEntry *selectRte = ExtractSelectRangeTableEntry(insertResultQuery);
	Oid targetRelationId = insertRte->relid;
	DistTableCacheEntry *targetCacheEntry = DistributedTableCacheEntry(targetRelationId);
	int shardCount = targetCacheEntry->shardIntervalArrayLength;
	int shardOffset = 0;
	uint32 taskIdIndex = 1;
	uint64 jobId = INVALID_JOB_ID;

	Relation distributedRelation = NULL;
	TupleDesc destTupleDescriptor = NULL;

	distributedRelation = heap_open(targetRelationId, RowExclusiveLock);
	destTupleDescriptor = RelationGetDescr(distributedRelation);

	/*
	 * If the type of insert column and target table's column type is
	 * different from each other. Cast insert column't type to target
	 * table's column
	 */
	AddInsertSelectCasts(insertSelectQuery->targetList, destTupleDescriptor);

	for (shardOffset = 0; shardOffset < shardCount; shardOffset++)
	{
		ShardInterval *targetShardInterval =
			targetCacheEntry->sortedShardIntervalArray[shardOffset];
		uint64 shardId = targetShardInterval->shardId;
		List *columnAliasList = NIL;
		List *insertShardPlacementList = NIL;
		Query *resultSelectQuery = NULL;
		StringInfo queryString = makeStringInfo();
		RelationShard *relationShard = NULL;
		Task *modifyTask = NULL;
		StringInfo resultId = makeStringInfo();

		/* during COPY, the shard ID is appended to the result name */
		appendStringInfo(resultId, "%s_" UINT64_FORMAT, resultIdPrefix, shardId);

		/* generate the query on the intermediate result */
		resultSelectQuery = BuildSubPlanResultQuery(insertSelectQuery->targetList,
													columnAliasList, resultId->data);

		/* put the intermediate result query in the INSERT..SELECT */
		selectRte->subquery = resultSelectQuery;

		/* setting an alias simplifies deparsing of RETURNING */
		if (insertRte->alias == NULL)
		{
			Alias *alias = makeAlias(CITUS_TABLE_ALIAS, NIL);
			insertRte->alias = alias;
		}

		/*
		 * Generate a query string for the query that inserts into a shard and reads
		 * from an intermediate result.
		 *
		 * Since CTEs have already been converted to intermediate results, they need
		 * to removed from the query. Otherwise, worker queries include both
		 * intermediate results and CTEs in the query.
		 */
		insertResultQuery->cteList = NIL;
		deparse_shard_query(insertResultQuery, targetRelationId, shardId, queryString);
		ereport(DEBUG2, (errmsg("distributed statement: %s", queryString->data)));

		LockShardDistributionMetadata(shardId, ShareLock);
		insertShardPlacementList = FinalizedShardPlacementList(shardId);

		relationShard = CitusMakeNode(RelationShard);
		relationShard->relationId = targetShardInterval->relationId;
		relationShard->shardId = targetShardInterval->shardId;

		modifyTask = CreateBasicTask(jobId, taskIdIndex, MODIFY_TASK, queryString->data);
		modifyTask->dependedTaskList = NULL;
		modifyTask->anchorShardId = shardId;
		modifyTask->taskPlacementList = insertShardPlacementList;
		modifyTask->relationShardList = list_make1(relationShard);
		modifyTask->replicationModel = targetCacheEntry->replicationModel;

		taskList = lappend(taskList, modifyTask);

		taskIdIndex++;
	}

	heap_close(distributedRelation, NoLock);

	return taskList;
}


/*
 * ExecutePlanIntoColocatedIntermediateResults executes the given PlannedStmt
 * and inserts tuples into a set of intermediate results that are colocated with
 * the target table for further processing of ON CONFLICT or RETURNING. It also
 * returns the hash of shard states that were used to insert tuplesinto the target
 * relation.
 */
static HTAB *
ExecutePlanIntoColocatedIntermediateResults(Oid targetRelationId,
											List *insertTargetList,
											PlannedStmt *selectPlan,
											EState *executorState,
											char *intermediateResultIdPrefix)
{
	ParamListInfo paramListInfo = executorState->es_param_list_info;
	int partitionColumnIndex = -1;
	List *columnNameList = NIL;
	bool stopOnFailure = false;
	char partitionMethod = 0;
	CitusCopyDestReceiver *copyDest = NULL;

	partitionMethod = PartitionMethod(targetRelationId);
	if (partitionMethod == DISTRIBUTE_BY_NONE)
	{
		stopOnFailure = true;
	}

	/* Get column name list and partition column index for the target table */
	columnNameList = BuildColumnNameListFromTargetList(targetRelationId,
													   insertTargetList);
	partitionColumnIndex = PartitionColumnIndexFromColumnList(targetRelationId,
															  columnNameList);

	/* set up a DestReceiver that copies into the intermediate table */
	copyDest = CreateCitusCopyDestReceiver(targetRelationId, columnNameList,
										   partitionColumnIndex, executorState,
										   stopOnFailure, intermediateResultIdPrefix);

	ExecutePlanIntoDestReceiver(selectPlan, paramListInfo, (DestReceiver *) copyDest);

	executorState->es_processed = copyDest->tuplesSent;

	XactModificationLevel = XACT_MODIFICATION_DATA;

	return copyDest->shardStateHash;
}


/*
 * ExecutePlanIntoRelation executes the given plan and inserts the
 * results into the target relation, which is assumed to be a distributed
 * table.
 */
static void
ExecutePlanIntoRelation(Oid targetRelationId, List *insertTargetList,
						PlannedStmt *selectPlan, EState *executorState)
{
	ParamListInfo paramListInfo = executorState->es_param_list_info;
	int partitionColumnIndex = -1;
	List *columnNameList = NIL;
	bool stopOnFailure = false;
	char partitionMethod = 0;
	CitusCopyDestReceiver *copyDest = NULL;

	partitionMethod = PartitionMethod(targetRelationId);
	if (partitionMethod == DISTRIBUTE_BY_NONE)
	{
		stopOnFailure = true;
	}

	/* Get column name list and partition column index for the target table */
	columnNameList = BuildColumnNameListFromTargetList(targetRelationId,
													   insertTargetList);
	partitionColumnIndex = PartitionColumnIndexFromColumnList(targetRelationId,
															  columnNameList);

	/* set up a DestReceiver that copies into the distributed table */
	copyDest = CreateCitusCopyDestReceiver(targetRelationId, columnNameList,
										   partitionColumnIndex, executorState,
										   stopOnFailure, NULL);

	ExecutePlanIntoDestReceiver(selectPlan, paramListInfo, (DestReceiver *) copyDest);

	executorState->es_processed = copyDest->tuplesSent;

	XactModificationLevel = XACT_MODIFICATION_DATA;
}


/*
 * BuildColumnNameListForCopyStatement build the column name list given the insert
 * target list.
 */
static List *
BuildColumnNameListFromTargetList(Oid targetRelationId, List *insertTargetList)
{
	ListCell *insertTargetCell = NULL;
	List *columnNameList = NIL;

	/* build the list of column names for the COPY statement */
	foreach(insertTargetCell, insertTargetList)
	{
		TargetEntry *insertTargetEntry = (TargetEntry *) lfirst(insertTargetCell);

		columnNameList = lappend(columnNameList, insertTargetEntry->resname);
	}

	return columnNameList;
}


/*
 * PartitionColumnIndexFromColumnList returns the index of partition column from given
 * column name list and relation ID. If given list doesn't contain the partition
 * column, it returns -1.
 */
static int
PartitionColumnIndexFromColumnList(Oid relationId, List *columnNameList)
{
	ListCell *columnNameCell = NULL;
	Var *partitionColumn = PartitionColumn(relationId, 0);
	int partitionColumnIndex = 0;

	foreach(columnNameCell, columnNameList)
	{
		char *columnName = (char *) lfirst(columnNameCell);

		AttrNumber attrNumber = get_attnum(relationId, columnName);

		/* check whether this is the partition column */
		if (partitionColumn != NULL && attrNumber == partitionColumn->varattno)
		{
			return partitionColumnIndex;
		}

		partitionColumnIndex++;
	}

	return -1;
}
