/*
 * Carried-load (garment) state (see sim/garments.h).
 *
 * A tiny pure transition table keyed by the machine kind that must drive each
 * step; no allocation, no time source, no SDL.
 */

#include "sim/garments.h"

#include <stddef.h>

int garmentRequiredKind(int state)
{
	switch (state) {
	case GARMENT_DIRTY:
		return MACHINE_KIND_WASHER;
	case GARMENT_WET_CLEAN:
		return MACHINE_KIND_DRYER;
	default:
		return -1;
	}
}

int garmentNextState(int state, int machineKind)
{
	switch (state) {
	case GARMENT_DIRTY:
		return machineKind == MACHINE_KIND_WASHER ? GARMENT_WET_CLEAN
							  : -1;
	case GARMENT_WET_CLEAN:
		return machineKind == MACHINE_KIND_DRYER ? GARMENT_DRY_CLEAN : -1;
	default:
		return -1;
	}
}

bool garmentCanAdvance(int state, int machineKind)
{
	return garmentNextState(state, machineKind) >= 0;
}

bool garmentAdvance(int *state, int machineKind)
{
	int next;

	if (state == NULL)
		return false;
	next = garmentNextState(*state, machineKind);
	if (next < 0)
		return false;
	*state = next;
	return true;
}

bool garmentStateValid(int state)
{
	return state >= 0 && state < GARMENT_COUNT;
}

const char *garmentStateName(int state)
{
	switch (state) {
	case GARMENT_DIRTY:
		return "DIRTY";
	case GARMENT_WET_CLEAN:
		return "WET_CLEAN";
	case GARMENT_DRY_CLEAN:
		return "DRY_CLEAN";
	default:
		return "?";
	}
}
