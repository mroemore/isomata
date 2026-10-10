/*
 * Interactable machines (see sim/machines.h).
 *
 * A fixed-capacity registry of FREE/CLAIMED/RUNNING/DONE/BROKEN machines with
 * exclusive claims and bounded wake events. No allocation, no time source, no
 * SDL: everything is deterministic and advances only by the caller's dt.
 */

#include "sim/machines.h"

#include <stddef.h>

void machinesInit(MachineRegistry *reg)
{
	if (reg == NULL)
		return;
	for (int i = 0; i < MACHINE_MAX; i++) {
		reg->slots[i] = (Machine){ 0 };
		reg->alive[i] = false;
	}
	reg->liveCount = 0;
}

MachineHandle machineCreate(MachineRegistry *reg, int kind, int tileX,
			    int tileZ, int fee, float runSecs)
{
	if (reg == NULL)
		return MACHINE_INVALID;
	for (int i = 0; i < MACHINE_MAX; i++) {
		Machine *m;

		if (reg->alive[i])
			continue;
		m = &reg->slots[i];
		*m = (Machine){ 0 };
		m->kind = kind;
		m->tileX = tileX;
		m->tileZ = tileZ;
		m->state = MACHINE_STATE_FREE;
		m->owner = MACHINE_NO_OWNER;
		m->fee = fee;
		m->runSecs = runSecs;
		m->runLeft = 0.0f;
		m->handle = i;
		reg->alive[i] = true;
		reg->liveCount++;
		return i;
	}
	return MACHINE_INVALID;
}

bool machineAlive(const MachineRegistry *reg, MachineHandle h)
{
	return reg != NULL && h >= 0 && h < MACHINE_MAX && reg->alive[h];
}

Machine *machineGet(MachineRegistry *reg, MachineHandle h)
{
	if (!machineAlive(reg, h))
		return NULL;
	return &reg->slots[h];
}

const Machine *machineGetConst(const MachineRegistry *reg, MachineHandle h)
{
	if (!machineAlive(reg, h))
		return NULL;
	return &reg->slots[h];
}

int machineCount(const MachineRegistry *reg)
{
	return reg == NULL ? 0 : reg->liveCount;
}

MachineHandle machineFirst(const MachineRegistry *reg)
{
	return machineNext(reg, MACHINE_INVALID);
}

MachineHandle machineNext(const MachineRegistry *reg, MachineHandle h)
{
	if (reg == NULL)
		return MACHINE_INVALID;
	for (int i = h + 1; i < MACHINE_MAX; i++)
		if (reg->alive[i])
			return i;
	return MACHINE_INVALID;
}

bool machineClaim(Machine *m, int owner)
{
	if (m == NULL || owner < MACHINE_OWNER_MIN)
		return false;
	if (m->state != MACHINE_STATE_FREE)
		return false;
	m->owner = owner;
	m->state = MACHINE_STATE_CLAIMED;
	return true;
}

bool machineStartRun(Machine *m)
{
	if (m == NULL || m->state != MACHINE_STATE_CLAIMED)
		return false;
	m->state = MACHINE_STATE_RUNNING;
	m->runLeft = m->runSecs;
	return true;
}

bool machineRelease(Machine *m)
{
	if (m == NULL)
		return false;
	if (m->state != MACHINE_STATE_CLAIMED &&
	    m->state != MACHINE_STATE_RUNNING &&
	    m->state != MACHINE_STATE_DONE)
		return false;
	m->state = MACHINE_STATE_FREE;
	m->owner = MACHINE_NO_OWNER;
	m->runLeft = 0.0f;
	return true;
}

bool machineBreak(Machine *m, MachineWake *out)
{
	bool hadOwner;

	if (m == NULL)
		return false;
	hadOwner = m->owner >= MACHINE_OWNER_MIN;
	/* A wake is only a real event when an owner was interrupted: with no
	 * owner, *out is stamped with machine == MACHINE_INVALID so the caller
	 * can tell "broke, nobody to wake" from "owner interrupted". */
	if (out != NULL) {
		out->machine = hadOwner ? m->handle : MACHINE_INVALID;
		out->owner = hadOwner ? m->owner : MACHINE_NO_OWNER;
		out->kind = MACHINE_WAKE_INTERRUPTED;
	}
	m->owner = MACHINE_NO_OWNER;
	m->state = MACHINE_STATE_BROKEN;
	m->runLeft = 0.0f;
	return true;
}

bool machineRepair(Machine *m)
{
	if (m == NULL || m->state != MACHINE_STATE_BROKEN)
		return false;
	m->state = MACHINE_STATE_FREE;
	m->owner = MACHINE_NO_OWNER;
	m->runLeft = 0.0f;
	return true;
}

int machinesUpdate(MachineRegistry *reg, float dt, MachineWake *outEvents,
		   int cap)
{
	int n = 0;

	if (reg == NULL)
		return 0;
	/* NaN-safe clamp: !(dt > 0) is true for NaN and negatives. */
	if (!(dt > 0.0f))
		dt = 0.0f;
	else if (dt > MACHINE_MAX_DT)
		dt = MACHINE_MAX_DT;

	for (int i = 0; i < MACHINE_MAX; i++) {
		Machine *m = &reg->slots[i];

		if (!reg->alive[i] || m->state != MACHINE_STATE_RUNNING)
			continue;
		m->runLeft -= dt;
		if (m->runLeft > 0.0f)
			continue;
		m->runLeft = 0.0f;
		m->state = MACHINE_STATE_DONE;
		/* Bounded append: a full buffer is a deterministic prefix, never
		 * an overflow. */
		if (outEvents != NULL && cap > 0 && n < cap) {
			outEvents[n].machine = m->handle;
			outEvents[n].owner = m->owner;
			outEvents[n].kind = MACHINE_WAKE_PHASE_DONE;
			n++;
		}
	}
	return n;
}

const char *machineStateName(int state)
{
	switch (state) {
	case MACHINE_STATE_FREE:
		return "FREE";
	case MACHINE_STATE_CLAIMED:
		return "CLAIMED";
	case MACHINE_STATE_RUNNING:
		return "RUNNING";
	case MACHINE_STATE_DONE:
		return "DONE";
	case MACHINE_STATE_BROKEN:
		return "BROKEN";
	default:
		return "?";
	}
}

const char *machineKindName(int kind)
{
	switch (kind) {
	case MACHINE_KIND_WASHER:
		return "WASHER";
	case MACHINE_KIND_DRYER:
		return "DRYER";
	default:
		return "?";
	}
}

const char *machineWakeKindName(int kind)
{
	switch (kind) {
	case MACHINE_WAKE_PHASE_DONE:
		return "PHASE_DONE";
	case MACHINE_WAKE_INTERRUPTED:
		return "INTERRUPTED";
	default:
		return "?";
	}
}
