#ifndef ISOMATA_AGENTS_NEED_H
#define ISOMATA_AGENTS_NEED_H

/*
 * The want ladder's arithmetic: a fixed-size need vector, its decay, and the
 * single urgency function every later decision is ordered by. See
 * agents/agent.h for the record that owns one of these vectors.
 *
 * MODEL. A Need is one named 0..1 quantity, read three ways:
 *   content  -> value near 1.0 means "satisfied".
 *   deficit  -> value near 0.0 means "urgent", via needBand.
 *   panic    -> a MODE CHANGE, a separate channel that goes above 0 while the
 *               need is under its threshold and bleeds off afterwards.
 * Keeping panic off the value channel is what lets "a dryer is on fire"
 * reprioritise an agent without a special case: world facts only move needs.
 *
 * SOFT FALLOFF, NOT GATING. wantUrgency is tierWeight * deficit, banded to
 * 0..NEED_BANDS-1 (integers, so two nearly-equal wants land in one band and
 * cannot displace each other — the anti-oscillation property). Tier weight
 * falls off with the Maslov-style tier, so a mild survival need LOSES to a
 * critical social one while a critical survival need always wins. The weights
 * are monotone non-increasing in SPEC tier, not in enum order: NEED_SATISFACTION
 * is spec tier 1 (being blocked is an inconvenience, not an emergency) and so
 * shares NEED_ENERGY's weight.
 *
 * DECAY. needDecay removes value at decayMul[n] * (1 + (1 - value) *
 * NEED_PANIC_RAMP) per second — the ramp makes an empty need run away faster
 * than a comfortable one, so urgency rises on its own. Each value is floored
 * at 0.0, never negative.
 *
 * NOT RESPONSIBLE FOR: what an agent does about a need (no activities, no
 * jobs, no errand adoption), who raises a panic (no world scan — influences
 * simply write value/panic), and per-role tuning (that is RoleProfile, see
 * agents/agent.h). This module has no idea an agent exists.
 *
 * Pure: no SDL, no allocation, no time source. dt is clamped by every entry
 * point, so a long stall advances a bounded amount and the whole vector is a
 * pure function of (state, dt) — run to run it is byte-identical.
 */

typedef enum Need {
	NEED_SURVIVAL,		/* spec tier 0 — physical safety, panic */
	NEED_ENERGY,		/* spec tier 1 — sustenance and safety */
	NEED_SOCIAL,		/* spec tier 2 — belonging, company */
	NEED_SATISFACTION,	/* spec tier 1 — being blocked / failing */
	NEED_ESTEEM,		/* spec tier 3 — competence, promises */
	NEED_FULFILMENT,	/* spec tier 4 — leisure, curiosity */
	NEED_COUNT		/* vector width: a compile-time constant */
} Need;

/* One agent's need vector. Two parallel fixed arrays, no pointers, so it
 * copies by value and is trivially inspectable for determinism checks. */
typedef struct NeedSet {
	float value[NEED_COUNT];	/* 1.0 = content, 0.0 = empty */
	float panic[NEED_COUNT];	/* 0.0 = calm, > 0.0 = panicking */
} NeedSet;

/* Urgency is an integer band, not a raw float: NEED_BANDS possible levels. */
#define NEED_BANDS 16
/* Every decay entry point clamps a frame's dt to at most this many seconds,
 * so a long stall advances a bounded amount (never a collapse). */
#define NEED_MAX_DT 0.1f
/* Extra decay acceleration per unit of deficit (the runaway-empty ramp). */
#define NEED_PANIC_RAMP 1.0f
/* Panic levels bleed off at this rate per second once the cause clears. */
#define NEED_PANIC_DECAY 0.5f
/* While ANY need panics, every non-panicking want is scaled by this, so a
 * panic outranks the filler it interrupts. */
#define NEED_PANIC_SUPPRESS 0.75f
/* The threshold needPanicDecay (the threshold-less entry point) holds panic
 * at: the shipped CUSTOMER/STAFF rows. Role-tuned roles go through
 * needPanicDecayRole with their own vector instead. */
#define NEED_PANIC_HOLD_DEFAULT 0.15f

/* Fill a vector with content: every value AND every panic channel 1.0.
 * NULL is a no-op.
 *
 * WARNING — the panic side of this is a birth condition, not a sane steady
 * state, and the two interact. A fresh vector is panicking on every need, so
 * wantUrgency returns NEED_BANDS - 1 for all six and the ladder cannot order
 * anything. It does NOT bleed off on its own: needPanicDecay holds a panic
 * while that need sits under its threshold, and a decaying need crosses the
 * threshold BEFORE the 0.5/s panic level reaches zero. Measured on a plain
 * CUSTOMER agent under agentsDecay, the panic settles at 0.70 and stays
 * there indefinitely, so the agent panics on all six needs forever. The
 * birth state is pinned here because the spec asks for it (panic 1.0 ==
 * content), but something upstream — agentCreate, or the spawn path — is
 * responsible for driving the panic channel down at birth, or the ladder is
 * dead for every agent it creates. */
void needSetInit(NeedSet *s);

/* Decay every need by its multiplier, ramped near empty, floored at 0.0.
 * dt is clamped to NEED_MAX_DT and a non-positive or NaN dt is a no-op.
 * decayMul == NULL is a no-op — a missing multiplier is not a licence to
 * guess one. */
void needDecay(NeedSet *s, const float decayMul[NEED_COUNT], float dt);

/* Bleed every panic level off at NEED_PANIC_DECAY per second, floored at
 * 0.0, EXCEPT while that need's value is under the module default threshold
 * (NEED_PANIC_HOLD_DEFAULT) — the cause persists, so the panic is held.
 * dt is clamped and NaN-safe like needDecay; panicThreshold == NULL means
 * every slot uses NEED_PANIC_HOLD_DEFAULT. */
void needPanicDecayRole(NeedSet *s, const float panicThreshold[NEED_COUNT],
			float dt);

/* needPanicDecay with the module default threshold vector. */
void needPanicDecay(NeedSet *s, float dt);

/* The band of one need: round(tierWeight * deficit * (NEED_BANDS - 1)),
 * always 0..NEED_BANDS-1. deficit is 1 - value clamped to [0, 1], so an
 * over-full need reads as content and an over-empty one as empty; the
 * product cannot leave the band range, so no output clamp is needed. An
 * unknown slot or a NULL set reads as band 0 (never a wild read). */
int needBand(const NeedSet *s, int need);

/* Bit n is set when panic[n] > 0.0f; 0 for a NULL set. */
int needPanicMask(const NeedSet *s);

/* The tier weight of a need, or 0.0f for an unknown slot. */
float needTierWeight(int need);

/* THE ordering function: 0..NEED_BANDS-1, higher wins.
 *   - the need's own panic outranks everything: NEED_BANDS - 1;
 *   - otherwise the band, scaled by NEED_PANIC_SUPPRESS and rounded, if ANY
 *     need is panicking (one panic pushes down every competing want);
 *   - otherwise the plain band.
 * NULL set or unknown slot reads 0. Pure and side-effect free: the same set
 * always yields the same number. */
int wantUrgency(const NeedSet *s, int need);

#endif