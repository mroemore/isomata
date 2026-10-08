#ifndef ISOMATA_EVENTS_H
#define ISOMATA_EVENTS_H

/*
 * Queued event bus for the engine's topics. Producers copy payloads in at
 * publish time; consumers receive plain events once per frame from the main
 * loop. This module is headless: no SDL, no GPU, no window.
 *
 * Topics (A topic groups events by owner; per-topic type constants live with
 * their owning modules, so `type` stays an opaque uint32_t here):
 *
 *	enum is the topic vocabulary, EV_TOPIC_COUNT is the X_COUNT sentinel
 *	(and is never a real topic).
 *
 * Concurrency and lifetime invariants:
 *
 * - publishEvent copies the payload into bus-owned storage. The caller may
 *   reuse or free its buffer as soon as publishEvent returns. The copy is
 *   freed after delivery; destroyEventBus frees copies that were never
 *   delivered.
 * - posting an event queues it; nothing is delivered until dispatchEvents.
 *   Capacity bounds the queue length, not payload size: when the queue is
 *   full publishEvent returns false.
 * - dispatchEvents delivers a snapshot: exactly the events queued when it
 *   was called, once each. An event published from inside a callback is
 *   delivered on the NEXT dispatchEvents call.
 * - dispatchEvents is not reentrant: a nested call (from inside a callback)
 *   returns immediately and delivers nothing.
 * - Subscription list changes from inside a callback are safe and defined:
 *   a subscriber removed after the dispatch began is not called for that
 *   dispatch, whether or not it already ran; a subscriber added after the
 *   dispatch began is first called on the NEXT dispatch. Subscriber
 *   callbacks run in subscription order.
 * - Within one callback, event->payload is valid to read (and event fields
 *   to copy); the pointer must not be freed or stored beyond the callback —
 *   retain via copy if needed.
 *
 * A dispatch sequence counter (rather than list mutation order) makes the
 * callback-safety rules hold: the bus stamps every subscription with the
 * dispatch it was born in and skips newborn records until the next top-level
 * dispatch, and unsubscribes mid-dispatch only deactivate a record.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum EventTopic {
	EV_TOPIC_ENGINE = 0,
	EV_TOPIC_GAMEPLAY,
	EV_TOPIC_UI,
	EV_TOPIC_AUDIO,
	EV_TOPIC_ACHIEVEMENT,
	EV_TOPIC_COUNT,
} EventTopic;

typedef struct Event Event;
typedef struct EventBus EventBus;

/* Sequence of one delivery: topic + owning module's type + payload copy. */
struct Event {
	uint32_t topic;
	uint32_t type;
	const void *payload;	/* read-only during the callback; may be NULL */
	size_t payloadSize;	/* bytes at payload; 0 means "no payload" */
};

/* ctx is the pointer given to subscribeEvent. Callbacks may publish,
 * subscribe, and unsubscribe freely (see invariants above). */
typedef void (*EventCallback)(void *ctx, const Event *event);

/* capacity is the bounded queue length; must be at least 1 (a bus that
 * cannot queue anything is rejected). */
EventBus *createEventBus(size_t capacity);
void destroyEventBus(EventBus *bus);

bool subscribeEvent(EventBus *bus, uint32_t topic,
		    EventCallback callback, void *ctx);
bool unsubscribeEvent(EventBus *bus, uint32_t topic,
		      EventCallback callback, void *ctx);
bool publishEvent(EventBus *bus, uint32_t topic, uint32_t type,
		  const void *payload, size_t payloadSize);
void dispatchEvents(EventBus *bus);

#endif /* ISOMATA_EVENTS_H */
