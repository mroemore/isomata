#ifndef ISOMATA_SIM_PLAN_H
#define ISOMATA_SIM_PLAN_H

/*
 * Plan templates (T23 layer 3): a goal expands to an ordered task list.
 *
 * This is a language-level "plan", NOT a general planner. Each goal owns a
 * static table of tasks; expanding a goal copies that table into a Plan the
 * brain executes one task at a time. The point of the table is data-driven
 * extensibility: adding a MACHINE KIND is a new kind constant plus a plan entry
 * that names it (the brain never switches on the kind — it reads the task's
 * `machineKind` and its garment transition), and adding a GOAL is a new table
 * plus an id. Adding a task KIND still needs an executor case, but the plan
 * table itself stays open.
 *
 * Pure: no SDL, no allocation, no time source. garments.h is included for the
 * GARMENT_* states the task transitions carry; machines.h for the MACHINE_KIND_*
 * values.
 *
 * The one goal the demo uses:
 *   LEAVE_WITH_CLEAN_DRY =
 *     USE_MACHINE(kind=WASHER, DIRTY     -> WET_CLEAN)
 *     USE_MACHINE(kind=DRYER,  WET_CLEAN -> DRY_CLEAN)
 *     LEAVE
 */

#include "sim/garments.h"
#include "sim/machines.h"

#include <stdbool.h>

/* The most tasks any goal may expand to. */
#define PLAN_MAX_TASKS 8

/* Goal ids. PLAN_GOAL_NONE is the sentinel an unknown/absent goal returns. */
#define PLAN_GOAL_NONE (-1)
#define PLAN_GOAL_LEAVE_WITH_CLEAN_DRY 0
#define PLAN_GOAL_COUNT 1

/* Task kinds. */
#define PLAN_TASK_USE_MACHINE 0
#define PLAN_TASK_LEAVE 1

typedef struct PlanTask {
	int kind;		/* PLAN_TASK_* */
	int machineKind;	/* USE_MACHINE payload: the kind to select */
	int garmentFrom;	/* USE_MACHINE payload: required load state */
	int garmentTo;		/* USE_MACHINE payload: load state after the run */
} PlanTask;

typedef struct Plan {
	PlanTask tasks[PLAN_MAX_TASKS];
	int count;
} Plan;

/* Expand `goal` into `*out`. Returns false (leaving `*out` untouched) for a
 * NULL `out` or an unknown goal. The plan is copied from a static table. */
bool planExpand(int goal, Plan *out);

/* Number of tasks in `p` (0 for a NULL plan). */
int planCount(const Plan *p);

/* The task at `index`, or NULL when out of range / NULL plan. */
const PlanTask *planTaskAt(const Plan *p, int index);

/* Stable log names: planGoalName: "NONE"/"LEAVE_WITH_CLEAN_DRY";
 * planTaskKindName: "USE_MACHINE"/"LEAVE". An unknown value returns "?". */
const char *planGoalName(int goal);
const char *planTaskKindName(int kind);

#endif /* ISOMATA_SIM_PLAN_H */
