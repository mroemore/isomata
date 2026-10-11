/*
 * The job board: fixed slots, exclusive expiring claims, band-ordered
 * selection (see agents/board.h for the model and the invariants).
 *
 * Straight flat arrays — the only algorithm worth stating is boardBestFor,
 * which walks the slots once and keeps the STRICTLY highest band, so the
 * lowest slot index wins a tie (the determinism rule).
 *
 * boardClaim below is the only line in the framework that stamps a CLAIMER
 * onto a job (`j->claimer = agentId`). Every other write of Job.claimer in
 * this file, and the one in jobInit, writes the unclaimed sentinel instead —
 * that is what makes "no job is double-claimed" checkable by reading this
 * file rather than by trusting a convention.
 *
 * Pure: no SDL, no allocation, no time source. Refusals print one line to
 * stderr (entities.c / mapsource.c convention) so a producer that believes it
 * enqueued or claimed something learns that it did not.
 */

#include "agents/board.h"

#include <stdio.h>
#include <string.h>

/* A live, non-terminal job that nobody but `agentId` holds. Terminal jobs
 * (DONE/FAILED) are excluded: re-running one is its owner's revive (a START
 * on the record), never a fresh selection. */
static bool jobAvailable(const JobBoard *b, int i, int agentId)
{
	const Job *j = &b->slots[i];

	if (!b->alive[i])
		return false;
	if (j->state == JOB_DONE || j->state == JOB_FAILED)
		return false;
	if (j->claimer == JOB_INVALID || j->claimer == agentId)
		return true;
	/* Somebody else holds it. A lapsed reservation is freed by
	 * boardExpire, which is the only clock in the module's contract, so
	 * a held job here is a LIVE hold. */
	return false;
}

static bool handleValid(const JobBoard *b, int handle)
{
	return b != NULL && handle >= 0 && handle < BOARD_MAX && b->alive[handle];
}

void boardInit(JobBoard *b, bool shared)
{
	if (b == NULL)
		return;
	/* Zeroes the slots and alive[] as well, so a re-initialised board is
	 * byte-identical to a fresh one. */
	memset(b, 0, sizeof(*b));
	b->shared = shared;
}

int boardEnqueue(JobBoard *b, const Job *j, int priority)
{
	int i;

	if (b == NULL || j == NULL) {
		fprintf(stderr, "board: enqueue rejected (null board/job)\n");
		return JOB_INVALID;
	}
	if (priority < 0 || priority >= JOB_PRIO_COUNT) {
		/* Provenance is data like any other: an unknown class says
		 * nothing about who may claim the job, so it is refused
		 * rather than guessed at. */
		fprintf(stderr, "board: enqueue rejected (priority %d)\n",
			priority);
		return JOB_INVALID;
	}
	for (i = 0; i < BOARD_MAX; i++) {
		Job *slot;

		if (b->alive[i])
			continue;
		slot = &b->slots[i];
		/* Scrub first: a reused slot must never show the previous
		 * occupant's job, even in a field nobody set. */
		*slot = (Job){ 0 };
		*slot = *j;
		/* ...and the claim is the BOARD's field, so a fresh job is
		 * nobody's whatever the producer's record happened to
		 * carry. */
		slot->claimer = JOB_INVALID;
		slot->claimExpiry = 0.0f;
		b->alive[i] = true;
		b->liveCount++;
		return i;
	}
	/* Full: a bounded drop, never an overflow. The producer sees
	 * JOB_INVALID and the floor sees why. */
	fprintf(stderr, "board: enqueue dropped, board full (%d jobs)\n",
		b->liveCount);
	return JOB_INVALID;
}

Job *boardJob(JobBoard *b, int handle)
{
	if (!handleValid(b, handle))
		return NULL;
	return &b->slots[handle];
}

bool boardRetire(JobBoard *b, int handle)
{
	Job *j = boardJob(b, handle);

	if (j == NULL)
		return false;
	*j = (Job){ 0 };
	/* A dead slot reads as an unclaimed, unbound job, not as a zeroed
	 * record: claimer 0 is agent 0, and target 0 is tile (0,0), so a
	 * reader that forgets alive[] would see a live-looking job. The
	 * sentinels are the ones jobInit writes. */
	j->band = -1;
	j->targetKind = JOB_INVALID;
	j->targetId = JOB_INVALID;
	j->claimer = JOB_INVALID;
	b->alive[handle] = false;
	b->liveCount--;
	return true;
}

bool boardClaim(JobBoard *b, int handle, int agentId)
{
	Job *j = boardJob(b, handle);

	if (j == NULL) {
		fprintf(stderr, "board: claim refused (no job %d)\n", handle);
		return false;
	}
	if (j->claimer != JOB_INVALID) {
		/* Held by somebody (possibly this agent): START is not a
		 * re-claim, and boardExpire is the only way a claim ends
		 * before its deadline. */
		return false;
	}
	j->claimer = agentId;
	j->claimExpiry = b->now + BOARD_RESERVE_SECS;
	return true;
}

void boardRelease(JobBoard *b, int handle)
{
	Job *j = boardJob(b, handle);

	if (j == NULL)
		return;
	j->claimer = JOB_INVALID;
	j->claimExpiry = 0.0f;
}

int boardExpire(JobBoard *b, float now)
{
	int released = 0;
	int i;

	if (b == NULL)
		return 0;
	/* A NaN clock is a broken clock: it neither advances the board nor
	 * evicts every claim on the floor. */
	if (!(now == now))
		return 0;
	b->now = now;
	for (i = 0; i < BOARD_MAX; i++) {
		Job *j = &b->slots[i];

		if (!b->alive[i] || j->claimer == JOB_INVALID)
			continue;
		/* A RESERVATION IS NOT A LEASE. The window frees a job
		 * nobody STARTED — the lifecycle still reads JOB_CLAIMED,
		 * "reserved, not yet walking" — so nobody sits on a job
		 * forever without beginning it.
		 *
		 * Once the job is under way (WALKING, INTERACTING, WAITING)
		 * the claim is the OWNER's and no sweep touches it, however
		 * long the job runs: a job may take as long as it takes, and
		 * the exclusivity that keeps one job from being executed
		 * twice must not expire underneath it. Such a claim ends the
		 * way every other one does — the job reaches DONE/FAILED and
		 * its owner releases it, or the owner releases it early
		 * (preemption, cancel, abandon). */
		if (j->state != JOB_CLAIMED)
			continue;
		/* AFTER the deadline, never at it: the claim is honoured
		 * for its full window. */
		if (now > j->claimExpiry) {
			j->claimer = JOB_INVALID;
			j->claimExpiry = 0.0f;
			released++;
		}
	}
	return released;
}

int boardBestFor(JobBoard *b, const int *skip, int agentId)
{
	int best = JOB_INVALID;
	int i;

	if (b == NULL)
		return JOB_INVALID;
	for (i = 0; i < BOARD_MAX; i++) {
		bool skipped = false;
		int s;

		if (!jobAvailable(b, i, agentId))
			continue;
		if (skip != NULL) {
			for (s = 0; skip[s] >= 0; s++) {
				if (skip[s] == i) {
					skipped = true;
					break;
				}
			}
			if (skipped)
				continue;
		}
		/* Strictly greater keeps the LOWEST slot index on a tie,
		 * which is what makes the answer deterministic. */
		if (best == JOB_INVALID || b->slots[i].band > b->slots[best].band)
			best = i;
	}
	return best;
}