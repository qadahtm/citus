/*-------------------------------------------------------------------------
 *
 * subplan_execution.h
 *
 * Functions for execution subplans.
 *
 * Copyright (c) 2012-2019, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#ifndef SUBPLAN_EXECUTION_H
#define SUBPLAN_EXECUTION_H


#include "distributed/multi_physical_planner.h"

extern int MaxIntermediateResult;
extern int SubPlanLevel;

extern void ExecuteSubPlans(DistributedPlan *distributedPlan);


#endif /* SUBPLAN_EXECUTION_H */
