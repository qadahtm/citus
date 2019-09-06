/*-------------------------------------------------------------------------
 *
 * shard_rebalancer.c
 *
 * Function definitions for the shard rebalancer tool.
 *
 * Copyright (c) 2019, Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"


#define NOT_SUPPORTED_IN_COMMUNITY(name) \
	PG_FUNCTION_INFO_V1(name); \
	Datum name(PG_FUNCTION_ARGS) { \
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
						errmsg(# name "() is only supported on Citus Enterprise"))); \
	}

NOT_SUPPORTED_IN_COMMUNITY(rebalance_table_shards);
NOT_SUPPORTED_IN_COMMUNITY(replicate_table_shards);
NOT_SUPPORTED_IN_COMMUNITY(get_rebalance_table_shards_plan);
NOT_SUPPORTED_IN_COMMUNITY(get_rebalance_progress);
