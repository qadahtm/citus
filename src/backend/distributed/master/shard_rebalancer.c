/*-------------------------------------------------------------------------
 *
 * shard_rebalancer.c
 *
 * Function definitions for the shard rebalancer tool.
 *
 * Copyright (c) 2014, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include <stdnoreturn.h>

/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(rebalance_table_shards);
PG_FUNCTION_INFO_V1(replicate_table_shards);
PG_FUNCTION_INFO_V1(get_rebalance_table_shards_plan);
PG_FUNCTION_INFO_V1(get_rebalance_progress);

static noreturn void
NotSupportedFunction(char *name)
{
	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s() is only supported on Citus Enterprise", name)));
}


/*
 * rebalance_table_shards rebalances the shards across the workers.
 */
Datum
rebalance_table_shards(PG_FUNCTION_ARGS)
{
	NotSupportedFunction("rebalance_table_shards");
}


/*
 * replicate_table_shards replicates under-replicated shards of the specified
 * table.
 */
Datum
replicate_table_shards(PG_FUNCTION_ARGS)
{
	NotSupportedFunction("replicate_table_shards");
}


/*
 * get_rebalance_table_shards_plan function calculates the shard move steps
 * required for the rebalance operations including the ones for colocated
 * tables.
 */
Datum
get_rebalance_table_shards_plan(PG_FUNCTION_ARGS)
{
	NotSupportedFunction("get_rebalance_table_shards_plan");
}


/*
 * get_rebalance_progress collects information about the ongoing rebalance operations and
 * returns the concatenated list of steps involved in the operations, along with their
 * progress information. Currently the progress field can take 4 integer values
 * (-1: error, 0: waiting, 1: moving, 2: moved). The progress field is of type bigint
 * because we may implement a more granular, byte-level progress as a future improvement.
 */
Datum
get_rebalance_progress(PG_FUNCTION_ARGS)
{
	NotSupportedFunction("get_rebalance_progress");
}
