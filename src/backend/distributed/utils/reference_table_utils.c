/*-------------------------------------------------------------------------
 *
 * reference_table_utils.c
 *
 * Declarations for public utility functions related to reference tables.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/genam.h"
#include "distributed/colocation_utils.h"
#include "distributed/commands.h"
#include "distributed/listutils.h"
#include "distributed/master_protocol.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_logical_planner.h"
#include "distributed/reference_table_utils.h"
#include "distributed/resource_lock.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/transaction_management.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_transaction.h"
#include "storage/lmgr.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/* local function forward declarations */
static void ReplicateSingleShardTableToAllWorkers(Oid relationId);
static void ReplicateShardToAllWorkers(ShardInterval *shardInterval);
static void ReplicateShardToNode(ShardInterval *shardInterval, char *nodeName,
								 int nodePort);
static void ConvertToReferenceTableMetadata(Oid relationId, uint64 shardId);

/* exports for SQL callable functions */
PG_FUNCTION_INFO_V1(upgrade_to_reference_table);


/*
 * upgrade_to_reference_table accepts a broadcast table which has only one shard and
 * replicates it across all nodes to create a reference table. It also modifies related
 * metadata to mark the table as reference.
 */
Datum
upgrade_to_reference_table(PG_FUNCTION_ARGS)
{
	Oid relationId = PG_GETARG_OID(0);
	List *shardIntervalList = NIL;
	ShardInterval *shardInterval = NULL;
	uint64 shardId = INVALID_SHARD_ID;
	DistTableCacheEntry *tableEntry = NULL;

	CheckCitusVersion(ERROR);
	EnsureCoordinator();
	EnsureTableOwner(relationId);

	if (!IsDistributedTable(relationId))
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot upgrade to reference table"),
						errdetail("Relation \"%s\" is not distributed.", relationName),
						errhint("Instead, you can use; "
								"create_reference_table('%s');", relationName)));
	}

	tableEntry = DistributedTableCacheEntry(relationId);

	if (tableEntry->partitionMethod == DISTRIBUTE_BY_NONE)
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot upgrade to reference table"),
						errdetail("Relation \"%s\" is already a reference table",
								  relationName)));
	}

	if (tableEntry->replicationModel == REPLICATION_MODEL_STREAMING)
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot upgrade to reference table"),
						errdetail("Upgrade is only supported for statement-based "
								  "replicated tables but \"%s\" is streaming replicated",
								  relationName)));
	}

	shardIntervalList = LoadShardIntervalList(relationId);
	if (list_length(shardIntervalList) != 1)
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot upgrade to reference table"),
						errdetail("Relation \"%s\" shard count is not one. Only "
								  "relations with one shard can be upgraded to "
								  "reference tables.", relationName)));
	}

	shardInterval = (ShardInterval *) linitial(shardIntervalList);
	shardId = shardInterval->shardId;

	LockShardDistributionMetadata(shardId, ExclusiveLock);
	LockShardResource(shardId, ExclusiveLock);

	ReplicateSingleShardTableToAllWorkers(relationId);

	PG_RETURN_VOID();
}


/*
 * ReplicateAllReferenceTablesToNode function finds all reference tables and
 * replicates them to the given worker node. It also modifies pg_dist_colocation
 * table to update the replication factor column when necessary. This function
 * skips reference tables if that node already has healthy placement of that
 * reference table to prevent unnecessary data transfer.
 */
void
ReplicateAllReferenceTablesToNode(char *nodeName, int nodePort)
{
	List *referenceTableList = ReferenceTableOidList();
	ListCell *referenceTableCell = NULL;
	uint32 workerCount = ActivePrimaryNodeCount();

	/* if there is no reference table, we do not need to replicate anything */
	if (list_length(referenceTableList) > 0)
	{
		List *referenceShardIntervalList = NIL;
		ListCell *referenceShardIntervalCell = NULL;

		/*
		 * We sort the reference table list to prevent deadlocks in concurrent
		 * ReplicateAllReferenceTablesToAllNodes calls.
		 */
		referenceTableList = SortList(referenceTableList, CompareOids);
		foreach(referenceTableCell, referenceTableList)
		{
			Oid referenceTableId = lfirst_oid(referenceTableCell);
			List *shardIntervalList = LoadShardIntervalList(referenceTableId);
			ShardInterval *shardInterval = (ShardInterval *) linitial(shardIntervalList);

			referenceShardIntervalList = lappend(referenceShardIntervalList,
												 shardInterval);
		}

		if (ClusterHasKnownMetadataWorkers())
		{
			BlockWritesToShardList(referenceShardIntervalList);
		}

		foreach(referenceShardIntervalCell, referenceShardIntervalList)
		{
			ShardInterval *shardInterval = (ShardInterval *) lfirst(
				referenceShardIntervalCell);
			uint64 shardId = shardInterval->shardId;

			LockShardDistributionMetadata(shardId, ExclusiveLock);

			ReplicateShardToNode(shardInterval, nodeName, nodePort);
		}

		/* create foreign constraints between reference tables */
		foreach(referenceShardIntervalCell, referenceShardIntervalList)
		{
			ShardInterval *shardInterval =
				(ShardInterval *) lfirst(referenceShardIntervalCell);
			char *tableOwner = TableOwner(shardInterval->relationId);
			List *commandList = CopyShardForeignConstraintCommandList(shardInterval);

			SendCommandListToWorkerInSingleTransaction(nodeName, nodePort,
													   tableOwner, commandList);
		}
	}

	/*
	 * Update replication factor column for colocation group of reference tables
	 * so that worker count will be equal to replication factor again.
	 */
	UpdateColocationGroupReplicationFactorForReferenceTables(workerCount);
}


/*
 * ReplicateSingleShardTableToAllWorkers accepts a broadcast table and replicates it to
 * all worker nodes. It assumes that caller of this function ensures that given broadcast
 * table has only one shard.
 */
static void
ReplicateSingleShardTableToAllWorkers(Oid relationId)
{
	List *shardIntervalList = LoadShardIntervalList(relationId);
	ShardInterval *shardInterval = (ShardInterval *) linitial(shardIntervalList);
	uint64 shardId = shardInterval->shardId;

	List *foreignConstraintCommandList = CopyShardForeignConstraintCommandList(
		shardInterval);

	if (foreignConstraintCommandList != NIL || TableReferenced(relationId))
	{
		char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot upgrade to reference table"),
						errdetail("Relation \"%s\" is part of a foreign constraint. "
								  "Foreign key constraints are not allowed "
								  "from or to reference tables.", relationName)));
	}

	/*
	 * ReplicateShardToAllWorkers function opens separate transactions (i.e., not part
	 * of any coordinated transactions) to each worker and replicates given shard to all
	 * workers. If a worker already has a healthy replica of given shard, it skips that
	 * worker to prevent copying unnecessary data.
	 */
	ReplicateShardToAllWorkers(shardInterval);

	/*
	 * We need to update metadata tables to mark this table as reference table. We modify
	 * pg_dist_partition, pg_dist_colocation and pg_dist_shard tables in
	 * ConvertToReferenceTableMetadata function.
	 */
	ConvertToReferenceTableMetadata(relationId, shardId);

	/*
	 * After the table has been officially marked as a reference table, we need to create
	 * the reference table itself and insert its pg_dist_partition, pg_dist_shard and
	 * existing pg_dist_placement rows.
	 */
	CreateTableMetadataOnWorkers(relationId);
}


/*
 * ReplicateShardToAllWorkers function replicates given shard to the all worker nodes
 * in separate transactions. While replicating, it only replicates the shard to the
 * workers which does not have a healthy replica of the shard. However, this function
 * does not obtain any lock on shard resource and shard metadata. It is caller's
 * responsibility to take those locks.
 */
static void
ReplicateShardToAllWorkers(ShardInterval *shardInterval)
{
	List *workerNodeList = NULL;
	ListCell *workerNodeCell = NULL;

	/* prevent concurrent pg_dist_node changes */
	LockRelationOid(DistNodeRelationId(), RowShareLock);

	workerNodeList = ActivePrimaryNodeList();

	/*
	 * We will iterate over all worker nodes and if healthy placement is not exist at
	 * given node we will copy the shard to that node. Then we will also modify
	 * the metadata to reflect newly copied shard.
	 */
	workerNodeList = SortList(workerNodeList, CompareWorkerNodes);
	foreach(workerNodeCell, workerNodeList)
	{
		WorkerNode *workerNode = (WorkerNode *) lfirst(workerNodeCell);
		char *nodeName = workerNode->workerName;
		uint32 nodePort = workerNode->workerPort;

		ReplicateShardToNode(shardInterval, nodeName, nodePort);
	}
}


/*
 * ReplicateShardToNode function replicates given shard to the given worker node
 * in a separate transaction. While replicating, it only replicates the shard to the
 * workers which does not have a healthy replica of the shard. This function also modifies
 * metadata by inserting/updating related rows in pg_dist_placement.
 */
static void
ReplicateShardToNode(ShardInterval *shardInterval, char *nodeName, int nodePort)
{
	uint64 shardId = shardInterval->shardId;

	bool missingOk = false;
	ShardPlacement *sourceShardPlacement = FinalizedShardPlacement(shardId, missingOk);
	char *srcNodeName = sourceShardPlacement->nodeName;
	uint32 srcNodePort = sourceShardPlacement->nodePort;
	bool includeData = true;
	List *ddlCommandList =
		CopyShardCommandList(shardInterval, srcNodeName, srcNodePort, includeData);

	List *shardPlacementList = ShardPlacementList(shardId);
	bool missingWorkerOk = true;
	ShardPlacement *targetPlacement = SearchShardPlacementInList(shardPlacementList,
																 nodeName, nodePort,
																 missingWorkerOk);
	char *tableOwner = TableOwner(shardInterval->relationId);

	/*
	 * Although this function is used for reference tables and reference table shard
	 * placements always have shardState = FILE_FINALIZED, in case of an upgrade of
	 * a non-reference table to reference table, unhealty placements may exist. In
	 * this case, we repair the shard placement and update its state in
	 * pg_dist_placement table.
	 */
	if (targetPlacement == NULL || targetPlacement->shardState != FILE_FINALIZED)
	{
		uint64 placementId = 0;
		int32 groupId = 0;

		ereport(NOTICE, (errmsg("Replicating reference table \"%s\" to the node %s:%d",
								get_rel_name(shardInterval->relationId), nodeName,
								nodePort)));

		SendCommandListToWorkerInSingleTransaction(nodeName, nodePort, tableOwner,
												   ddlCommandList);
		if (targetPlacement == NULL)
		{
			groupId = GroupForNode(nodeName, nodePort);

			placementId = GetNextPlacementId();
			InsertShardPlacementRow(shardId, placementId, FILE_FINALIZED, 0, groupId);
		}
		else
		{
			groupId = targetPlacement->groupId;
			placementId = targetPlacement->placementId;
			UpdateShardPlacementState(placementId, FILE_FINALIZED);
		}

		/*
		 * Although ReplicateShardToAllWorkers is used only for reference tables,
		 * during the upgrade phase, the placements are created before the table is
		 * marked as a reference table. All metadata (including the placement
		 * metadata) will be copied to workers after all reference table changed
		 * are finished.
		 */
		if (ShouldSyncTableMetadata(shardInterval->relationId))
		{
			char *placementCommand = PlacementUpsertCommand(shardId, placementId,
															FILE_FINALIZED, 0,
															groupId);

			SendCommandToWorkers(WORKERS_WITH_METADATA, placementCommand);
		}
	}
}


/*
 * ConvertToReferenceTableMetadata accepts a broadcast table and modifies its metadata to
 * reference table metadata. To do this, this function updates pg_dist_partition,
 * pg_dist_colocation and pg_dist_shard. This function assumes that caller ensures that
 * given broadcast table has only one shard.
 */
static void
ConvertToReferenceTableMetadata(Oid relationId, uint64 shardId)
{
	uint32 currentColocationId = TableColocationId(relationId);
	uint32 newColocationId = CreateReferenceTableColocationId();
	Var *distributionColumn = NULL;
	char shardStorageType = ShardStorageType(relationId);
	text *shardMinValue = NULL;
	text *shardMaxValue = NULL;

	/* delete old metadata rows */
	DeletePartitionRow(relationId);
	DeleteColocationGroupIfNoTablesBelong(currentColocationId);
	DeleteShardRow(shardId);

	/* insert new metadata rows */
	InsertIntoPgDistPartition(relationId, DISTRIBUTE_BY_NONE, distributionColumn,
							  newColocationId, REPLICATION_MODEL_2PC);
	InsertShardRow(relationId, shardId, shardStorageType, shardMinValue, shardMaxValue);
}


/*
 * CreateReferenceTableColocationId creates a new co-location id for reference tables and
 * writes it into pg_dist_colocation, then returns the created co-location id. Since there
 * can be only one colocation group for all kinds of reference tables, if a co-location id
 * is already created for reference tables, this function returns it without creating
 * anything.
 */
uint32
CreateReferenceTableColocationId()
{
	uint32 colocationId = INVALID_COLOCATION_ID;
	List *workerNodeList = ActivePrimaryNodeList();
	int shardCount = 1;
	int replicationFactor = list_length(workerNodeList);
	Oid distributionColumnType = InvalidOid;

	/* check for existing colocations */
	colocationId = ColocationId(shardCount, replicationFactor, distributionColumnType);
	if (colocationId == INVALID_COLOCATION_ID)
	{
		colocationId = CreateColocationGroup(shardCount, replicationFactor,
											 distributionColumnType);
	}

	return colocationId;
}


/*
 * DeleteAllReferenceTablePlacementsFromNodeGroup function iterates over list of reference
 * tables and deletes all reference table placements from pg_dist_placement table
 * for given group. However, it does not modify replication factor of the colocation
 * group of reference tables. It is caller's responsibility to do that if it is necessary.
 */
void
DeleteAllReferenceTablePlacementsFromNodeGroup(int32 groupId)
{
	List *referenceTableList = ReferenceTableOidList();
	List *referenceShardIntervalList = NIL;
	ListCell *referenceTableCell = NULL;

	/* if there are no reference tables, we do not need to do anything */
	if (list_length(referenceTableList) == 0)
	{
		return;
	}

	/*
	 * We sort the reference table list to prevent deadlocks in concurrent
	 * DeleteAllReferenceTablePlacementsFromNodeGroup calls.
	 */
	referenceTableList = SortList(referenceTableList, CompareOids);
	if (ClusterHasKnownMetadataWorkers())
	{
		referenceShardIntervalList = GetSortedReferenceShardIntervals(referenceTableList);

		BlockWritesToShardList(referenceShardIntervalList);
	}

	foreach(referenceTableCell, referenceTableList)
	{
		GroupShardPlacement *placement = NULL;
		StringInfo deletePlacementCommand = makeStringInfo();

		Oid referenceTableId = lfirst_oid(referenceTableCell);
		List *placements = GroupShardPlacementsForTableOnGroup(referenceTableId,
															   groupId);
		if (list_length(placements) == 0)
		{
			/* this happens if the node was previously disabled */
			continue;
		}

		placement = (GroupShardPlacement *) linitial(placements);

		LockShardDistributionMetadata(placement->shardId, ExclusiveLock);

		DeleteShardPlacementRow(placement->placementId);

		appendStringInfo(deletePlacementCommand,
						 "DELETE FROM pg_dist_placement WHERE placementid = "
						 UINT64_FORMAT,
						 placement->placementId);
		SendCommandToWorkers(WORKERS_WITH_METADATA, deletePlacementCommand->data);
	}
}


/*
 * ReferenceTableOidList function scans pg_dist_partition to create a list of all
 * reference tables. To create the list, it performs sequential scan. Since it is not
 * expected that this function will be called frequently, it is OK not to use index scan.
 * If this function becomes performance bottleneck, it is possible to modify this function
 * to perform index scan.
 */
List *
ReferenceTableOidList()
{
	List *distTableOidList = DistTableOidList();
	ListCell *distTableOidCell = NULL;

	List *referenceTableList = NIL;

	foreach(distTableOidCell, distTableOidList)
	{
		DistTableCacheEntry *cacheEntry = NULL;
		Oid relationId = lfirst_oid(distTableOidCell);

		cacheEntry = DistributedTableCacheEntry(relationId);

		if (cacheEntry->partitionMethod == DISTRIBUTE_BY_NONE)
		{
			referenceTableList = lappend_oid(referenceTableList, relationId);
		}
	}

	return referenceTableList;
}


/* CompareOids is a comparison function for sort shard oids */
int
CompareOids(const void *leftElement, const void *rightElement)
{
	Oid *leftId = (Oid *) leftElement;
	Oid *rightId = (Oid *) rightElement;

	if (*leftId > *rightId)
	{
		return 1;
	}
	else if (*leftId < *rightId)
	{
		return -1;
	}
	else
	{
		return 0;
	}
}
