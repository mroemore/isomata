#ifndef ISOMATA_SIM_GARMENTS_H
#define ISOMATA_SIM_GARMENTS_H

/*
 * Carried-load (garment) state for the laundromat sim (T23 layer 3).
 *
 * A customer carries exactly one load through a small linear chain:
 *   DIRTY --(a WASHER run completes)--> WET_CLEAN --(a DRYER run completes)--> DRY_CLEAN
 * DRY_CLEAN is the terminal state and the goal's end condition; the *goal* is
 * complete only when the load is DRY_CLEAN AND the customer has left the
 * building (the brain, sim/brain.c, owns the departure half — this module owns
 * the load state alone).
 *
 * Pure: no SDL, no allocation, no time source. It includes sim/machines.h only
 * for the MACHINE_KIND_* constants the transition validation needs (that header
 * is itself dependency-free).
 *
 * KIND / TRANSITION VALIDATION. Every transition names the machine KIND that
 * must drive it: only a WASHER turns DIRTY into WET_CLEAN and only a DRYER
 * turns WET_CLEAN into DRY_CLEAN. A machine whose kind does not match the
 * needed transition is rejected (false, state untouched), so the brain can
 * never apply the wrong run to a load — the pin the brief asks for.
 */

#include "sim/machines.h"

#include <stdbool.h>

typedef enum GarmentState {
	GARMENT_DIRTY = 0,	/* as the customer arrives */
	GARMENT_WET_CLEAN = 1,	/* a washer run completed */
	GARMENT_DRY_CLEAN = 2,	/* a dryer run completed: the goal's end state */
	GARMENT_COUNT = 3,
} GarmentState;

/* The machine kind that advances `state` to its next state, or -1 when `state`
 * is terminal (DRY_CLEAN) or unknown. Pure; no registry access. */
int garmentRequiredKind(int state);

/* The state a run on `machineKind` produces from `state`, or -1 when that kind
 * cannot advance `state` (a kind/transition mismatch, a terminal or unknown
 * state). Pure. */
int garmentNextState(int state, int machineKind);

/* True when a run on `machineKind` is a legal next step for `state`. */
bool garmentCanAdvance(int state, int machineKind);

/* Apply a run on `machineKind` in place: on success write the next state to
 * `*state` and return true. On a kind/transition mismatch, a terminal/unknown
 * state, or a NULL `state`, leave `*state` untouched and return false. */
bool garmentAdvance(int *state, int machineKind);

/* True when `state` is one of the defined GarmentState values. */
bool garmentStateValid(int state);

/* Stable log name: "DIRTY" / "WET_CLEAN" / "DRY_CLEAN"; "?" for anything else. */
const char *garmentStateName(int state);

#endif /* ISOMATA_SIM_GARMENTS_H */
