/*
 * The want ladder's arithmetic (see agents/need.h for the model).
 *
 * PANIC IS AN ALARM, NOT A CONTENT QUANTITY. It starts at 0.0 and is raised
 * by the world acting on a need. It is also RESERVED FOR TIER-0 SURVIVAL:
 * a need whose panic threshold is negative never panics at all, because the
 * spec's reprioritising-alarm story is "a dryer is on fire", and an agent
 * that panicked on every emptied need would suppress its whole ladder and
 * be just as inert. An emptied non-survival need is still maximally urgent
 * through its tier weight — the band is what orders wants, not the alarm.
 *
 * Pure: no SDL, no allocation, no time source, no libm. Every entry point
 * clamps dt the way entities.c does — `!(dt > 0.0f)` catches zero, negative
 * AND NaN in one comparison, so a bad frame can never advance a need or, far
 * worse, poison it with NaN that then poisons every band derived from it.
 */

#include "agents/need.h"

#include <stddef.h>

/* The soft-falloff curve: monotone non-increasing in SPEC tier (0 survival,
 * 1 sustenance + satisfaction, 2 belonging, 3 esteem, 4 fulfilment), which is
 * deliberately NOT enum order — NEED_SATISFACTION shares tier 1's weight
 * because being blocked is an inconvenience, not an emergency. Indexed by
 * slot so the table stays a plain row-per-need. */
static const float g_tierWeight[NEED_COUNT] = {
	[NEED_SURVIVAL]		= 1.00f,
	[NEED_ENERGY]		= 0.60f,
	[NEED_SOCIAL]		= 0.40f,
	[NEED_SATISFACTION]	= 0.60f,
	[NEED_ESTEEM]		= 0.25f,
	[NEED_FULFILMENT]	= 0.15f
};

/* The module default threshold vector: panic is reserved for tier-0
 * survival, so survival carries the real threshold and every other slot is
 * the negative "reserved" marker. Written out in full because a designated
 * initialiser would leave the rest at 0.0f — a REAL threshold, i.e. the
 * exact opposite of the reservation. */
static const float g_defaultPanicThreshold[NEED_COUNT] = {
	[NEED_SURVIVAL]		= NEED_PANIC_HOLD_DEFAULT,
	[NEED_ENERGY]		= -1.0f,
	[NEED_SOCIAL]		= -1.0f,
	[NEED_SATISFACTION]	= -1.0f,
	[NEED_ESTEEM]		= -1.0f,
	[NEED_FULFILMENT]	= -1.0f
};

/* An unknown slot carries no weight and reads as band 0, so a bad index can
 * never invent urgency — and never reads out of bounds either. */
static int needSlotValid(int need)
{
	return need >= 0 && need < NEED_COUNT;
}

void needSetInit(NeedSet *s)
{
	int n;

	if (s == NULL)
		return;
	/* Content and calm: panic is an alarm the world raises, not a birth
	 * condition. Starting it high would pin every want at its maximum and
	 * never recover. */
	for (n = 0; n < NEED_COUNT; n++) {
		s->value[n] = 1.0f;
		s->panic[n] = 0.0f;
	}
}

void needDecay(NeedSet *s, const float decayMul[NEED_COUNT], float dt)
{
	int n;

	if (s == NULL || decayMul == NULL || !(dt > 0.0f))
		return;
	if (dt > NEED_MAX_DT)
		dt = NEED_MAX_DT;
	for (n = 0; n < NEED_COUNT; n++) {
		float v = s->value[n];
		/* Deceleration accelerates near empty: the runaway ramp is what
		 * makes an ignored need get louder on its own. */
		float rate = decayMul[n] *
			     (1.0f + (1.0f - v) * NEED_PANIC_RAMP);
		float next = v - rate * dt;

		/* Floored, never negative: a need that reached empty stays
		 * empty instead of climbing back through zero. */
		if (next < 0.0f)
			next = 0.0f;
		s->value[n] = next;
	}
}

void needPanicDecayRole(NeedSet *s, const float panicThreshold[NEED_COUNT],
			float dt)
{
	int n;

	if (s == NULL || !(dt > 0.0f))
		return;
	if (dt > NEED_MAX_DT)
		dt = NEED_MAX_DT;
	for (n = 0; n < NEED_COUNT; n++) {
		float threshold = (panicThreshold != NULL)
					? panicThreshold[n]
					: g_defaultPanicThreshold[n];
		float next;

		/* RESERVED (negative threshold): this need is not allowed to
		 * panic, so it neither holds, nor rises, nor bleeds — its
		 * channel is left exactly as it stands. Testing the threshold
		 * SIGN first keeps the reservation explicit: routing it
		 * through the value comparison below would work by accident
		 * (an empty 0.0 still compares >= -1.0) and would start
		 * quietly decaying as soon as someone wrote a panic level by
		 * hand. */
		if (threshold < 0.0f)
			continue;
		if (s->value[n] < threshold) {
			/* Cause live: the alarm climbs toward full. Without the
			 * rise an emptied need would band high and then sit
			 * there forever, and the panic mode would never be
			 * reachable in a running game. */
			next = s->panic[n] + NEED_PANIC_RISE * dt;
			/* Ceiling 1.0f. !(x <= y) also folds NaN up to the
			 * ceiling rather than letting it through. */
			if (!(next <= 1.0f))
				next = 1.0f;
		} else {
			/* Cause cleared: the alarm bleeds off toward nothing. */
			next = s->panic[n] - NEED_PANIC_DECAY * dt;
			if (!(next > 0.0f))
				next = 0.0f;
		}
		s->panic[n] = next;
	}
}

void needPanicDecay(NeedSet *s, float dt)
{
	needPanicDecayRole(s, NULL, dt);
}

int needBand(const NeedSet *s, int need)
{
	float deficit;
	float raw;

	if (s == NULL || !needSlotValid(need))
		return 0;
	deficit = 1.0f - s->value[need];
	/* !(x > 0.0f) also rejects NaN: an unreadable value reads as content
	 * instead of casting an undefined (int)NaN. */
	if (!(deficit > 0.0f))
		deficit = 0.0f;
	else if (deficit > 1.0f)
		deficit = 1.0f;
	raw = g_tierWeight[need] * deficit;
	/* weight <= 1.0 and deficit <= 1.0 bound raw by 1.0, so raw * 15 + 0.5
	 * stays within [0.5, 15.5] and the truncation is already in band. */
	return (int)(raw * (NEED_BANDS - 1) + 0.5f);
}

int needPanicMask(const NeedSet *s)
{
	int mask = 0;
	int n;

	if (s == NULL)
		return 0;
	for (n = 0; n < NEED_COUNT; n++)
		if (s->panic[n] > 0.0f)
			mask |= 1 << n;
	return mask;
}

float needTierWeight(int need)
{
	if (!needSlotValid(need))
		return 0.0f;
	return g_tierWeight[need];
}

int wantUrgency(const NeedSet *s, int need)
{
	int band;

	if (s == NULL || !needSlotValid(need))
		return 0;
	/* A need that panics itself is maximal whatever its value: the mode
	 * change is read from the panic channel, never the value channel. */
	if (s->panic[need] > 0.0f)
		return NEED_BANDS - 1;
	band = needBand(s, need);
	if (needPanicMask(s) == 0)
		return band;
	/* Someone is panicking, so this want steps down — scaled by a factor
	 * below 1 and rounded AFTER scaling, which keeps a band that rounding
	 * would leave alone unchanged. */
	return (int)((float)band * NEED_PANIC_SUPPRESS + 0.5f);
}