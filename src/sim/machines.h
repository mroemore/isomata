#ifndef ISOMATA_SIM_MACHINES_H
#define ISOMATA_SIM_MACHINES_H

/*
 * Interactable machines: the laundromat sim's layer-1 objects (T22).
 *
 * Pure: no SDL, no allocation, no time source, no entity/render dependency.
 * The registry is a caller-owned fixed array of MACHINE_MAX slots; a handle is
 * a slot index gated by an `alive[]` map, exactly like the entity registry.
 *
 * MODEL. A machine owns a tile and a small state machine; an entity walks ONTO
 * the tile to interact (v1 convention: the machine occupies its tile but the
 * tile stays walkable — placement is the level's call, this module only stores
 * the tile). The owner field carries the handle of the interacting entity
 * (an integer, matching EntityHandle from entities/entities.h; this module does
 * not include that header so it stays dependency-free).
 *
 * STATES. FREE (no owner, claimable) / CLAIMED (one owner bound, run not
 * started) / RUNNING (the owner's timed phase is counting down) / DONE (the
 * timed phase completed; the owner may still collect and release) / BROKEN
 * (refuses every claim until repaired). Only FREE admits a claim.
 *
 * EXCLUSIVITY. machineClaim succeeds only from FREE. Every occupied state
 * (CLAIMED/RUNNING/DONE) and BROKEN refuse it, so two claims in the same tick —
 * the second sees the first's CLAIMED state — cannot both win. The refusal IS
 * the guarantee the T23 brain relies on to bind an owner safely.
 *
 * WAKE EVENTS. A machine wakes its owner in exactly two moments: when a
 * RUNNING timer completes (PHASE_DONE) and when a machine with an owner is
 * broken (INTERRUPTED). Wakes are appended to a caller buffer by
 * machinesUpdate / machineBreak and may be dropped (never overflowed) if the
 * buffer is full — `cap` bounds them and the return counts what was written.
 * The T21 executor FSM consumes these as FSM_EVENT_PHASE_DONE /
 * FSM_EVENT_INTERRUPTED.
 *
 * ILLEGAL TRANSITIONS. Every operation is NULL-safe and rejects an illegal
 * transition by returning false with the machine untouched (claim from an
 * occupied/broken state, start-run without a claim, release of FREE/BROKEN,
 * repair of a non-broken machine). machineBreak is the one always-allowed op
 * (any state -> BROKEN).
 */

#include <stdbool.h>

/* Registry capacity (slots). Fixed, no allocation. */
#define MACHINE_MAX 16
/* An invalid / dead handle. */
#define MACHINE_INVALID (-1)
/* The owner value of a machine with no owner (equal to ENTITY_INVALID; kept as
 * a separate name so call sites read the machine's intent). */
#define MACHINE_NO_OWNER (-1)
/* machineClaim rejects an owner below this, so a machine can never be bound to
 * an invalid entity handle. */
#define MACHINE_OWNER_MIN 0

/* machinesUpdate clamps a frame's dt to at most this many seconds, matching the
 * entity walker's bounded-step rule (a long stall advances a bounded amount). */
#define MACHINE_MAX_DT 0.1f

typedef int MachineHandle;	/* 0..MACHINE_MAX-1, or MACHINE_INVALID */

typedef enum MachineState {
	MACHINE_STATE_FREE = 0,
	MACHINE_STATE_CLAIMED = 1,
	MACHINE_STATE_RUNNING = 2,
	MACHINE_STATE_DONE = 3,
	MACHINE_STATE_BROKEN = 4,
	MACHINE_STATE_COUNT = 5,
} MachineState;

/* Known kinds. Deliberately NOT an enum lock-in: `kind` is an int and the brain
 * filters by equality; these constants name the two the demo uses. */
#define MACHINE_KIND_WASHER 0
#define MACHINE_KIND_DRYER 1

typedef enum MachineWakeKind {
	MACHINE_WAKE_PHASE_DONE = 0,	/* a RUNNING timer completed */
	MACHINE_WAKE_INTERRUPTED = 1,	/* the owner's machine broke */
} MachineWakeKind;

typedef struct Machine {
	int kind;		/* free-form int (MACHINE_KIND_* are the knowns) */
	int tileX;
	int tileZ;
	int state;		/* MachineState */
	int owner;		/* entity handle, or MACHINE_NO_OWNER */
	int fee;		/* cost data for the brain's PAY step (T23) */
	float runSecs;		/* the machine's own run duration (data) */
	float runLeft;		/* RUNNING countdown; 0 otherwise */
	MachineHandle handle;	/* this slot's index, copied for wake payloads */
} Machine;

/* One wake: the completed/broken machine, its owner and the wake kind. */
typedef struct MachineWake {
	MachineHandle machine;
	int owner;		/* entity handle */
	int kind;		/* MachineWakeKind */
} MachineWake;

typedef struct MachineRegistry {
	Machine slots[MACHINE_MAX];
	bool alive[MACHINE_MAX];
	int liveCount;
} MachineRegistry;

/* Reset the registry to empty. NULL is a no-op. */
void machinesInit(MachineRegistry *reg);

/* Allocate a slot and return its handle, or MACHINE_INVALID when the registry
 * is full / NULL. The new machine is FREE with owner MACHINE_NO_OWNER, the
 * given run duration, and runLeft 0. `kind`/`tileX`/`tileZ`/`fee` are stored
 * verbatim (no validation — the level owns placement). */
MachineHandle machineCreate(MachineRegistry *reg, int kind, int tileX,
			    int tileZ, int fee, float runSecs);

/* True when `h` names a live machine. */
bool machineAlive(const MachineRegistry *reg, MachineHandle h);

/* Mutable / const pointer to a live machine, or NULL for a NULL registry / dead
 * handle. */
Machine *machineGet(MachineRegistry *reg, MachineHandle h);
const Machine *machineGetConst(const MachineRegistry *reg, MachineHandle h);

/* Live machine count (0 for NULL). */
int machineCount(const MachineRegistry *reg);

/* Iterate live handles in slot order:
 *   for (MachineHandle h = machineFirst(reg); h != MACHINE_INVALID;
 *        h = machineNext(reg, h)) ...
 */
MachineHandle machineFirst(const MachineRegistry *reg);
MachineHandle machineNext(const MachineRegistry *reg, MachineHandle h);

/* --- Transitions (pure; NULL-safe; illegal => false + no change) --------- */

/* FREE -> CLAIMED, binding `owner` (which must be >= MACHINE_OWNER_MIN).
 * Refused (false) for NULL, an occupied state (CLAIMED/RUNNING/DONE), BROKEN,
 * or an invalid owner. This is the exclusivity guarantee. */
bool machineClaim(Machine *m, int owner);

/* CLAIMED -> RUNNING, starting the runSecs timer (runLeft = runSecs). Refused
 * for NULL or a non-CLAIMED state. */
bool machineStartRun(Machine *m);

/* CLAIMED / RUNNING / DONE -> FREE, clearing the owner and the timer. Refused
 * for NULL, FREE or BROKEN. */
bool machineRelease(Machine *m);

/* Any state -> BROKEN (idempotent: breaking a BROKEN machine returns true and
 * changes nothing). When the machine had a valid owner, the owner is cleared
 * and, if `out` is non-NULL, an INTERRUPTED wake is written to *out (machine =
 * this handle, owner, MACHINE_WAKE_INTERRUPTED). With no owner, *out is still
 * stamped but with machine == MACHINE_INVALID, so the caller can distinguish
 * "broke, nobody to wake" from a real interrupt. Returns false only for a NULL
 * `m`. */
bool machineBreak(Machine *m, MachineWake *out);

/* BROKEN -> FREE, clearing the owner. Refused for NULL or a non-BROKEN state. */
bool machineRepair(Machine *m);

/* --- Timers + wakes ---------------------------------------------------- */

/* Advance every RUNNING machine's timer by `dt` seconds (clamped to
 * [0, MACHINE_MAX_DT], NaN-safe). A RUNNING machine whose timer reaches <= 0
 * (exactly at t == runSecs) becomes DONE and appends a PHASE_DONE wake (with
 * its handle and owner) to outEvents. Wakes are appended in slot order until
 * `cap` is reached; an overflow is silently dropped (a deterministic prefix)
 * rather than overflowed. Returns the number of wakes written (0 for a NULL
 * registry or a NULL outEvents / cap <= 0). */
int machinesUpdate(MachineRegistry *reg, float dt, MachineWake *outEvents,
		   int cap);

/* Stable names for logging: "FREE"/"CLAIMED"/"RUNNING"/"DONE"/"BROKEN",
 * "WASHER"/"DRYER", "PHASE_DONE"/"INTERRUPTED". An unknown value returns "?". */
const char *machineStateName(int state);
const char *machineKindName(int kind);
const char *machineWakeKindName(int kind);

#endif /* ISOMATA_SIM_MACHINES_H */
