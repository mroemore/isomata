/*
 * Plan templates (see sim/plan.h).
 *
 * Goals are static tables; planExpand copies one into the caller's Plan. Pure:
 * no allocation, no SDL, no time source.
 */

#include "sim/plan.h"

#include <stddef.h>

/* The LEAVE_WITH_CLEAN_DRY goal: wash, then dry, then leave. The garment
 * transitions are the plan's data — the brain applies them and validates them
 * against the machine kind via sim/garments.h. */
static const PlanTask kLeaveWithCleanDry[] = {
	{ PLAN_TASK_USE_MACHINE, MACHINE_KIND_WASHER, GARMENT_DIRTY,
	  GARMENT_WET_CLEAN },
	{ PLAN_TASK_USE_MACHINE, MACHINE_KIND_DRYER, GARMENT_WET_CLEAN,
	  GARMENT_DRY_CLEAN },
	{ PLAN_TASK_LEAVE, 0, 0, 0 },
};

bool planExpand(int goal, Plan *out)
{
	const PlanTask *src;
	int count;
	int i;

	if (out == NULL)
		return false;
	switch (goal) {
	case PLAN_GOAL_LEAVE_WITH_CLEAN_DRY:
		src = kLeaveWithCleanDry;
		count = (int)(sizeof(kLeaveWithCleanDry) /
			      sizeof(kLeaveWithCleanDry[0]));
		break;
	default:
		return false;
	}
	if (count > PLAN_MAX_TASKS)
		count = PLAN_MAX_TASKS;
	for (i = 0; i < count; i++)
		out->tasks[i] = src[i];
	out->count = count;
	return true;
}

int planCount(const Plan *p)
{
	return p == NULL ? 0 : p->count;
}

const PlanTask *planTaskAt(const Plan *p, int index)
{
	if (p == NULL || index < 0 || index >= p->count)
		return NULL;
	return &p->tasks[index];
}

const char *planGoalName(int goal)
{
	switch (goal) {
	case PLAN_GOAL_NONE:
		return "NONE";
	case PLAN_GOAL_LEAVE_WITH_CLEAN_DRY:
		return "LEAVE_WITH_CLEAN_DRY";
	default:
		return "?";
	}
}

const char *planTaskKindName(int kind)
{
	switch (kind) {
	case PLAN_TASK_USE_MACHINE:
		return "USE_MACHINE";
	case PLAN_TASK_LEAVE:
		return "LEAVE";
	default:
		return "?";
	}
}
