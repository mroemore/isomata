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
 * WHY THE CLAIM EXPIRES. A reservation is a promise to START a job, not to
 * finish it: if it is not started inside the window it goes back on the
 * board, so nobody sits on a job forever and a dead worker cannot deadlock
 * the floor (the stale-claim failure the spec names). The window is honoured
 * for its full length — a claim is released only AFTER its deadline, never
 * at it. There is deliberately no renewal in this task: a job that runs
 * longer than the window keeps running (the lifecycle owns it) but no longer
 * holds the slot, and the activity layer re-claims when it needs a longer
 * lease.
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
 * Refused (false, job untouched) unless the job is UNCLAIMED: another
 * agent's live claim, the holder's own re-claim, a NULL board, an
 * out-of-range handle and a dead slot all refuse. boardExpire is the only
 * thing that turns a held job back into a claimable one. */
bool boardClaim(JobBoard *b, int handle, int agentId);

/* Give slot `handle` back: `claimer` and `claimExpiry` clear, the job stays
 * enqueued and alive. This is the cancel-releases-resources rule — a
 * preempted or abandoned job goes back on the board rather than vanishing.
 * Releasing a job nobody holds is a no-op. NULL is a no-op. */
void boardRelease(JobBoard *b, int handle);

/* Stamp the board's clock to `now` and free every claim whose deadline has
 * passed; returns how many claims were RELEASED (0 for a NULL board).
 *
 * Release happens only AFTER the deadline, so a claim is honoured for its
 * full window. A NaN `now` releases nothing and stamps nothing: a broken
 * clock must not evict every claim on the floor. */
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