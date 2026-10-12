#ifndef ISOMATA_AGENTS_BOARD_H
#define ISOMATA_AGENTS_BOARD_H

/*
 * The job board: where jobs wait, and the exclusive, expiring claim that
 * decides who may take one. See agents/job.h for the record a board holds and
 * agents/agent.h for the agent that owns one.
 *
 * MODEL. A board is BOARD_MAX flat slots plus an `alive[]` gate and a live
 * count — the same shape as the entity and agent registries, for the same
 * reasons: no allocation, a handle is a slot index, a retired slot is
 * reused, and two live jobs never alias. A job is stored BY VALUE: a board
 * is a table of records, not of pointers. Handles are always the LOWEST free
 * index, so a board's slot order is dense and meaningful (the tie-break rule
 * below depends on it).
 *
 * TWO KINDS OF BOARD, ONE RECORD. `shared` is the whole difference:
 *   personal (shared = false) — the agent's own errands' jobs. One consumer,
 *     so nothing contends and nothing is really "claimed" (the owner may
 *     hold what it queued; nobody else ever sees the board).
 *   shared (shared = true)    — work anybody may pull. This is where the
 *     claim matters, and where two agents can want the same job in the same
 *     tick.
 *
 * STAFF PULL FROM THE SHARED BOARD; EVERYBODY ELSE DOES NOT. There is
 * deliberately NO ROLE GATE IN THIS MODULE, and there still is not one:
 * the spec says staff pull from the shared board, and board.c knows nothing
 * about roles — it does not include agents/agent.h, which includes this one.
 * The gate therefore lives with the agents, in the two places a shared job
 * can actually be picked up: agentsUpdate's scan and agentsClaimFromShared,
 * both on ROLE_STAFF (agents/agent.h). Task 3 shipped that scan ungated and
 * named this as the seat; this note is the seat filled.
 *
 * Nothing is lost by the gate being elsewhere: a role decides WHO may claim
 * (agentsClaimFromShared asks before it claims), and a claim is still decided
 * by the exclusivity below. What a board must never do is ask "is this agent
 * a customer", because that is a question about the agent, not about the work
 * waiting here.
 * The personal board is embedded in Agent BY VALUE (agents/agent.h), so
 * "personal" is literal rather than an index into a pool: AGENT_MAX x
 * BOARD_MAX x sizeof(Job) of static memory, and nothing else. That is the
 * price of not allocating, and sizeof(Job) is pinned by the board suite
 * because a field added back to Job is a multi-megabyte regression.
 *
 * THE CLAIM IS THE EXCLUSIVITY GUARANTEE. boardClaim succeeds only on a job
 * nobody holds: it stamps `claimer` and an ABSOLUTE deadline
 * (board->now + BOARD_RESERVE_SECS) and refuses everything else, including
 * a re-claim by the holder — a second START on a live job is exactly what an
 * exclusive claim exists to prevent.
 *
 * `Job.claimer` has exactly one writer in the whole framework, and it is not
 * a convention, it is the grep: boardClaim is the only code that puts a
 * CLAIMER'S ID in the field (src/agents/board.c, `j->claimer = agentId`).
 * Everything else writes the UNCLAIMED sentinel JOB_INVALID and nothing else
 * — jobInit on a brand-new record, boardEnqueue's scrub, boardRelease,
 * boardExpire and boardRetire. No other module mentions the field at all.
 *
 * A CLAIM IS A RESERVATION, NOT A LEASE. This distinction is the whole
 * contract of boardClaim and boardExpire, so it is worth stating flatly:
 *
 *   reserved (the lifecycle is still JOB_CLAIMED — "reserved, not yet
 *   walking") — the claim is a PROMISE TO START. If the job has not left
 *   JOB_CLAIMED by the end of BOARD_RESERVE_SECS, boardExpire frees it and
 *   it goes back on the board, so nobody sits on a job forever and a worker
 *   that stopped before starting cannot deadlock the floor (the stale-claim
 *   failure the spec names). The window is honoured for its full length: a
 *   reservation is released only AFTER its deadline, never at it.
 *
 *   running (WALKING, INTERACTING, WAITING) — the claim is the OWNER'S and
 *   no sweep HERE touches it, however long the job takes. A 30-second machine
 *   wash must not become claimable halfway through, or a second agent
 *   starts executing the same job: that is double execution, and it breaks
 *   the one guarantee this module exists to give. A running claim ends the
 *   way every claim ends — the job reaches DONE/FAILED and its owner
 *   releases it (boardRelease), or the owner releases it early on
 *   preemption, cancel or abandon.
 *
 *   WHICH IS WHY A DEAD OWNER'S RUNNING CLAIM IS NOT THIS MODULE'S TO FIND.
 *   A worker destroyed mid-job leaves a claim nothing here can expire — no
 *   deadline is supposed to. That one is healed a layer up, by the pass that
 *   hands the work out: agentsClaimFromShared releases every claim whose
 *   owner id is no longer in the registry (agents/agent.c). The rule stays
 *   here because the boundary is here: a board must never ask whether an
 *   agent is alive — it does not know what an agent is — but it must say
 *   plainly which failures it does not cover.
 *
 * No renewal mechanism exists because none is needed: a running claim does
 * not expire, and a reservation that is never converted into a running claim
 * expires on its own.
 *
 * TIME IS THE CALLER'S. A board has no clock of its own: boardExpire is the
 * single way time enters one, and it does two things in that order — it
 * stamps the board's clock (which boardClaim reads to compute a deadline)
 * and then frees every claim whose deadline has passed, returning how many it
 * released. So a claim can never be stale for longer than one tick of a
 * caller that sweeps, and the board stays a pure function of its call
 * sequence.
 *
 * ORDERING IS THE BAND, NEVER THE PROVENANCE. A priority class says who may
 * claim a job (errand / assigned / fill), which is the spec's "provenance
 * only decides may this agent claim it, never ordering". boardBestFor reads
 * `band` off the record and takes the STRICTLY highest, which keeps the
 * LOWEST slot index on a tie: the same input always returns the same job,
 * run to run. Bands are integers (agents/need.h) precisely so two nearly
 * equal jobs land in one band and cannot displace each other; the switch
 * margin that consumes them is in agentShouldSwitch (agents/agent.h).
 *
 * NOT RESPONSIBLE FOR: what a job is (agents/job.h), who should run it
 * (agents/need.h + agents/agent.h), moving anything (no entities here), the
 * clock (the caller supplies it), and persistence. boardEnqueue's `priority`
 * is validated and not stored: nothing may order by it, so a board that
 * remembered it would invite exactly the provenance-as-priority bug the
 * spec warns about. It lands here as the board's own data.
 *
 * Pure: no SDL, no allocation, no time source. A refusal is a return value
 * plus a stderr diagnostic (the same convention as entities.c and
 * mapsource.c), never an overflow and never a silent drop of a caller that
 * believes it enqueued something.
 */

#include "agents/job.h"

#include <stdbool.h>

/* Board capacity (slots). Fixed, no allocation. */
#define BOARD_MAX 128
/* The reservation window: how long a claim may sit un-started before the
 * board frees it. See "WHY THE CLAIM EXPIRES" above. */
#define BOARD_RESERVE_SECS 5.0f

/* Board provenance — who may claim from here, NEVER how the jobs order.
 * The enum is validated at enqueue (an unknown class is refused) and then
 * not stored; see the header note on ordering. */
typedef enum JobPriority {
	JOB_PRIO_ERRAND,	/* the agent's own committed goal work */
	JOB_PRIO_ASSIGNED,	/* work off the shared board */
	JOB_PRIO_FILL,		/* self-generated secondary activity */
	JOB_PRIO_COUNT
} JobPriority;

typedef struct JobBoard {
	Job slots[BOARD_MAX];
	bool alive[BOARD_MAX];
	int liveCount;
	bool shared;		/* false = personal: one consumer, no contention */
	float now;		/* the caller's clock, stamped by boardExpire */
} JobBoard;

/* The boards an agent tick is handed: every agent's personal board lives in
 * its own Agent record, so this carries the ONE board they share. */
typedef struct AgentBoards {
	JobBoard shared;
} AgentBoards;

/* Reset a board to empty and record which kind it is. `now` starts at 0.0f, so
 * a fresh board's clock reads "no time has passed". NULL is a no-op. */
void boardInit(JobBoard *b, bool shared);

/* Copy `j` into the lowest free slot and return that handle, or JOB_INVALID
 * on a refusal (NULL board/job, a priority class outside JobPriority, or a
 * full board) — a bounded drop with a stderr diagnostic, never an overflow.
 *
 * The slot is scrubbed before the copy, and the two fields the BOARD owns are
 * forced to their unclaimed values whatever the caller's record carried: a
 * reused slot must never show the previous occupant's claim. Everything else
 * (kind, target, band, duration, served[]) is stored verbatim. */
int boardEnqueue(JobBoard *b, const Job *j, int priority);

/* The job in slot `handle`, or NULL for a NULL board, an out-of-range handle
 * or a dead slot. The pointer is invalidated by boardRetire on that slot. */
Job *boardJob(JobBoard *b, int handle);

/* Drop slot `handle`: alive[] clears, the slot is scrubbed, liveCount drops.
 * This is the reclaim path for a job nobody will run again (finished,
 * abandoned), so a long-lived board does not fill up with dead rows — the
 * bounded drop above is a safety net, not a way to recycle. Returns false
 * for a NULL board or an unknown / already dead handle. */
bool boardRetire(JobBoard *b, int handle);

/* Take slot `handle` for `agentId`: stamps `claimer` and
 * `claimExpiry = b->now + BOARD_RESERVE_SECS`.
 *
 * Refused (false, job untouched) unless nobody holds the job: another
 * agent's claim, the holder's own re-claim, a NULL board, an out-of-range
 * handle and a dead slot all refuse.
 *
 * The stamp is a RESERVATION, and the owner owns the rest of the contract:
 * START the job it just claimed (jobStep -> JOB_CLAIMED, after which the
 * sweep will NOT take the claim back, however long the job runs), or give the
 * claim back with boardRelease.
 *
 * A claim on a job whose lifecycle never left JOB_UNCLAIMED is the one case
 * neither rule covers: the sweep only frees JOB_CLAIMED, so it stays until its
 * owner releases it. agentsUpdate always claims and STARTs in the same
 * breath, so such a claim is a caller that has stalled, and boardRelease is
 * how it is cleaned up. */
bool boardClaim(JobBoard *b, int handle, int agentId);

/* Give slot `handle` back: `claimer` and `claimExpiry` clear, the job stays
 * enqueued and alive (its lifecycle state is NOT touched — a WALKING job that
 * is released is walked by whoever takes it next). This is the
 * cancel-releases-resources rule: a preempted or abandoned job goes back on
 * the board rather than vanishing.
 *
 * This is the ONLY way a running job's claim ends before the job itself
 * finishes. Releasing a job nobody holds is a no-op. NULL is a no-op. */
void boardRelease(JobBoard *b, int handle);

/* Stamp the board's clock to `now` and free every claim whose deadline has
 * passed AND whose job is still sitting in JOB_CLAIMED; returns how many
 * claims were RELEASED (0 for a NULL board).
 *
 * THE RULE, exactly: a claim is freed only while the job is RESERVED (state
 * JOB_CLAIMED, "claimed but not yet started"). A job that has moved on to
 * WALKING, INTERACTING or WAITING keeps its claim for as long as it runs — a
 * long job is not a stale claim, and expiring one would let a second agent
 * start executing the same job. Those claims end through boardRelease (or by
 * the job finishing, which its owner then releases).
 *
 * Release happens only AFTER the deadline, never at it, so a reservation is
 * honoured for its full window. A NaN `now` releases nothing and stamps
 * nothing: a broken clock must not evict every claim on the floor. */
int boardExpire(JobBoard *b, float now);

/* The best job on this board for `agentId`: the highest band among live,
 * non-terminal jobs that nobody else holds, ties broken on the lowest slot
 * index. Returns JOB_INVALID when there is none (NULL board included).
 *
 * `skip` is a sentinel-terminated list of handles the caller is already
 * handling — the engaged job, mostly — and may be NULL; any negative entry
 * ends the list, and a stale or out-of-range entry in it is inert rather
 * than fatal. A job held by `agentId` is still a candidate (it is that
 * agent's own work); one held by anybody else is not, which is what makes a
 * shared board exclusive without a second data structure. */
int boardBestFor(JobBoard *b, const int *skip, int agentId);

#endif /* ISOMATA_AGENTS_BOARD_H */