/*
 * Event bus implementation. See events.h for the invariant block: copied
 * payloads, snapshot dispatch, and the dispatch-sequence rules that make
 * subscription changes from inside callbacks defined behavior.
 *
 * Storage:
 * - queue: fixed ring of capacity slots, each holding one event plus its
 *   payload copy. head/count track the live span.
 * - subs: growable append-only record list. unsubscribes during a running
 *   dispatch deactivate (dead-mark) instead of shifting, so no callback can
 *   pull the iteration cursor off track. Dead records are compacted at
 *   dispatch entry; a new subscriber always APPENDS (a freed slot is never
 *   reused), which is what keeps subscription order observable.
 */

#include "events.h"

#include <stdlib.h>
#include <string.h>

typedef struct QueuedEvent {
	Event ev;
	void *copy;		/* payload copy; NULL when payloadSize == 0 */
} QueuedEvent;

typedef struct Subscriber {
	uint32_t topic;
	uint32_t born;		/* dispatchSeq when subscribed; newborn
				 * records (born == dispatchSeq of the
				 * running dispatch) are skipped until the
				 * next top-level dispatch */
	EventCallback callback;
	void *ctx;
	bool dead;		/* unsubscribed; skipped until compaction */
} Subscriber;

struct EventBus {
	QueuedEvent *queue;
	size_t capacity;
	size_t head;
	size_t count;

	Subscriber *subs;
	size_t subCount;
	size_t subCap;

	uint32_t dispatchSeq;	/* bumped at every top-level dispatch */
	bool dispatching;
};

EventBus *createEventBus(size_t capacity)
{
	EventBus *bus;

	if (capacity == 0)
		return NULL;
	bus = calloc(1, sizeof(*bus));
	if (bus == NULL)
		return NULL;
	bus->queue = calloc(capacity, sizeof(*bus->queue));
	if (bus->queue == NULL) {
		free(bus);
		return NULL;
	}
	bus->capacity = capacity;
	return bus;
}

void destroyEventBus(EventBus *bus)
{
	size_t i;

	if (bus == NULL)
		return;
	for (i = 0; i < bus->count; i++)
		free(bus->queue[(bus->head + i) % bus->capacity].copy);
	free(bus->queue);
	free(bus->subs);
	free(bus);
}

bool subscribeEvent(EventBus *bus, uint32_t topic,
		    EventCallback callback, void *ctx)
{
	Subscriber *slot;
	size_t cap;
	Subscriber *grown;

	if (bus == NULL || callback == NULL)
		return false;

	/* Append-only: subscriber callbacks run in subscription order, a
	 * documented invariant. Dead records are never reused as insert
	 * slots — even a slot freed by the running dispatch must not be
	 * fed to a new subscriber, or the newcomer would deliver ahead of
	 * older live records on the next dispatch. Space for dead records
	 * is recovered by the compaction at every dispatch entry, and the
	 * record array grows only when live records have exhausted it;
	 * no capacity limit is intended here. */
	if (bus->subCount == bus->subCap) {
		cap = bus->subCap == 0 ? 4 : bus->subCap * 2;
		grown = realloc(bus->subs, cap * sizeof(*grown));
		if (grown == NULL)
			return false;
		bus->subs = grown;
		bus->subCap = cap;
	}
	slot = &bus->subs[bus->subCount++];
	slot->topic = topic;
	slot->callback = callback;
	slot->ctx = ctx;
	slot->born = bus->dispatchSeq;
	slot->dead = false;
	return true;
}

bool unsubscribeEvent(EventBus *bus, uint32_t topic,
		      EventCallback callback, void *ctx)
{
	size_t i;

	if (bus == NULL)
		return false;

	for (i = 0; i < bus->subCount; i++) {
		Subscriber *s = &bus->subs[i];

		if (s->dead || s->topic != topic ||
		    s->callback != callback || s->ctx != ctx)
			continue;
		if (bus->dispatching) {
			/* Mid-dispatch the list must not shift under the
			 * delivery loop: deactivate only. */
			s->dead = true;
		} else {
			/* Outside a dispatch: compact immediately. */
			memmove(&bus->subs[i], &bus->subs[i + 1],
				(bus->subCount - i - 1) * sizeof(*bus->subs));
			bus->subCount--;
		}
		return true;
	}
	return false;
}

bool publishEvent(EventBus *bus, uint32_t topic, uint32_t type,
		  const void *payload, size_t payloadSize)
{
	QueuedEvent *entry;

	if (bus == NULL)
		return false;
	if (bus->count == bus->capacity)
		return false;	/* bounded: capacity limits length, not size */
	if (payloadSize > 0 && payload == NULL)
		return false;

	entry = &bus->queue[(bus->head + bus->count) % bus->capacity];
	entry->ev.topic = topic;
	entry->ev.type = type;
	entry->ev.payload = NULL;
	entry->ev.payloadSize = payloadSize;
	entry->copy = NULL;
	if (payloadSize > 0) {
		void *copy = malloc(payloadSize);

		if (copy == NULL)
			return false;
		memcpy(copy, payload, payloadSize);
		entry->ev.payload = copy;
		entry->copy = copy;
	}
	bus->count++;
	return true;
}

/* Deliver one event to every live subscriber born before this dispatch.
 * The record is copied before the call: the callback may subscribe into a
 * grown array or unsubscribe (it can only dead-mark; the list never shifts
 * mid-dispatch), so no cursor adjustment is ever needed. */
static void deliverToSubscribers(EventBus *bus, const Event *ev)
{
	size_t i;

	for (i = 0; i < bus->subCount; i++) {
		Subscriber s = bus->subs[i];

		if (s.dead || s.born >= bus->dispatchSeq ||
		    s.topic != ev->topic)
			continue;
		s.callback(s.ctx, ev);
	}
}

void dispatchEvents(EventBus *bus)
{
	size_t limit;
	size_t k;

	if (bus == NULL || bus->dispatching)
		return;		/* not reentrant */

	/* Drop dead records from previous dispatches before stamping the new
	 * sequence; mid-dispatch removals only deactivate above. */
	{
		size_t r, w = 0;

		for (r = 0; r < bus->subCount; r++)
			if (!bus->subs[r].dead)
				bus->subs[w++] = bus->subs[r];
		bus->subCount = w;
	}

	bus->dispatchSeq++;
	bus->dispatching = true;

	/* Snapshot semantics: deliver exactly what is queued now. Events
	 * published by callbacks land behind the cursor (head+count) and are
	 * picked up by the next top-level dispatch. */
	limit = bus->count;
	for (k = 0; k < limit; k++) {
		QueuedEvent entry = bus->queue[bus->head];

		bus->head = (bus->head + 1) % bus->capacity;
		bus->count--;

		deliverToSubscribers(bus, &entry.ev);
		free(entry.copy);
	}

	bus->dispatching = false;
}
