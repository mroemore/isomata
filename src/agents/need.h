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
/* ...and climb at this rate per second while the cause is live, so an
 * emptied need actually enters panic instead of merely sitting there. */
#define NEED_PANIC_RISE 0.5f
/* While ANY need panics, every non-panicking want is scaled by this, so a
 * panic outranks the filler it interrupts. */
#define NEED_PANIC_SUPPRESS 0.75f
/* The default threshold for a panicking-CAPABLE need (the survival slot).
 * A NEGATIVE threshold is the reserved marker: that need never panics at
 * all. */
#define NEED_PANIC_HOLD_DEFAULT 0.15f

/* Fill a vector with content: every value 1.0 and every panic channel 0.0.
 * NULL is a no-op.
 *
 * Panic starts CLEAR on purpose. Panic is an alarm level, not a content
 * quantity — the spec's dryer-fire example raises it from the outside, and a
 * born panic of 1.0 would pin wantUrgency at NEED_BANDS - 1 for all six
 * needs and leave the ladder inert forever (a decaying need crosses its
 * threshold before the panic level could ever bleed off). A fresh agent is
 * content and calm, so its wants band and order normally from tick one. */
void needSetInit(NeedSet *s);

/* Decay every need by its multiplier, ramped near empty, floored at 0.0.
 * dt is clamped to NEED_MAX_DT and a non-positive or NaN dt is a no-op.
 * decayMul == NULL is a no-op — a missing multiplier is not a licence to
 * guess one. */
void needDecay(NeedSet *s, const float decayMul[NEED_COUNT], float dt);

/* Move every panic level one tick: RISE at NEED_PANIC_RISE per second while
 * the need sits under its threshold (the cause is live, so the alarm
 * climbs), and BLEED at NEED_PANIC_DECAY once it is back above (the cause
 * cleared). The rise is clamped at 1.0f and the bleed floored at 0.0f.
 *
 * A NEGATIVE threshold reserves the need: it never rises, never holds, and
 * its panic channel is left exactly as it stands (0.0f from needSetInit).
 * Panic is the tier-0 mechanism — the spec's dryer-fire example reprioritises
 * everyone through survival — so every non-survival slot is reserved. An
 * emptied non-survival need is still fully urgent through its tier weight; it
 * just never sets off the alarm, because a satisfied-in-nothing agent that
 * panics on everything is the same inert ladder by another route.
 *
 * ARM ONLY TIER-0 NEEDS. Panic is a maximal, suppressing override: the
 * panicking need returns NEED_BANDS - 1 outright, and every OTHER want is
 * scaled by NEED_PANIC_SUPPRESS while it holds. Arming a lower-tier need
 * therefore inverts the ladder — an emptied NEED_FULFILMENT panicking reads 15
 * while an emptied, unarmed NEED_SURVIVAL is pushed from 15 down to 11, so the
 * agent abandons a critical survival need for a leisure one. That is exactly
 * what the tier weights exist to prevent, and the override does not care about
 * them. Unreachable through the three shipped role profiles, which reserve every
 * non-survival need — but needPanicDecayRole is public, so it is one row in the
 * role table away. Change the role table before arming anything else.
 *
 * dt is clamped to NEED_MAX_DT and a non-positive or NaN dt is a no-op.
 * panicThreshold == NULL means every slot uses the module default vector
 * (NEED_PANIC_HOLD_DEFAULT for survival, reserved for the rest). */
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
