/*
 * Event bus tests (CTOL rung 1: unit + boundary).
 *
 * Covers: copied payload ownership, publish deferred until dispatchEvents,
 * multiple subscribers, unsubscribe during callback (peer and self),
 * bounded queue overflow, subscribe during callback, reentrant dispatch,
 * null/edge arguments, and destroy hygiene (undelivered payloads freed).
 * Pure: links only events.c plus the Unity subset; no SDL.
 *
 * Harness convention: this file belongs to the ONE headless executable
 * (test_pure). It must not define main()/setUp()/tearDown(); it leans on
 * unity's weak no-op fixtures, owns its state per test, and exposes a single
 * run_test_events() that test_main.c declares and calls. Tests create their
 * own bus so no fixture or cross-test state is shared.
 */

#include "unity.h"

#include "events.h"

#include <stdlib.h>
#include <string.h>

/* --- helpers ---------------------------------------------------------- */

static int g_calls;

static void cb_count(void *ctx, const Event *event)
{
	(void)ctx;
	(void)event;
	g_calls++;
}

/* Subscriber identity is passed by pointer to an int slot; deliveries are
 * recorded in subscription order. */
static int id_a = 0x0A;
static int id_b = 0x0B;
static int id_c = 0x0C;

static int g_order[8];
static int g_order_n;

static void cb_order(void *ctx, const Event *event)
{
	(void)event;
	g_order[g_order_n++] = *(const int *)ctx;
}

/* Payload expectations carried through ctx. Every subscriber of a topic
 * sees EVERY event of that topic, so expectations are keyed by event type. */
struct Expect {
	const char *text42;
	const char *text43;
	const void *prov42;	/* buffer the type-42 event was published with */
	const void *prov43;	/* buffer the type-43 event was published with */
	int hits;
};

static void cb_check_payload(void *ctx, const Event *event)
{
	struct Expect *e = ctx;
	const char *text;
	const void *prov;

	TEST_ASSERT_NOT_NULL(event);
	TEST_ASSERT_EQUAL_UINT32(EV_TOPIC_UI, event->topic);
	if (event->type == 42u) {
		text = e->text42;
		prov = e->prov42;
	} else if (event->type == 43u) {
		text = e->text43;
		prov = e->prov43;
	} else {
		TEST_FAIL_MESSAGE("unexpected event type");
		return;
	}
	TEST_ASSERT_EQUAL_UINT64((uint64_t)strlen(text) + 1,
				 (uint64_t)event->payloadSize);
	TEST_ASSERT_EQUAL_STRING(text, (const char *)event->payload);
	TEST_ASSERT_NOT_NULL(event->payload);
	/* The payload is a private copy; it must not alias the buffer the
	 * caller published with. */
	TEST_ASSERT_FALSE(event->payload == prov);
	e->hits++;
}

/* Callback that unsubscribes another subscriber (cb_order @ peerCtx). */
struct UnsubPeer {
	EventBus *bus;
	void *peerCtx;
};

static void cb_unsubscribes_peer(void *ctx, const Event *event)
{
	const struct UnsubPeer *p = ctx;

	(void)event;
	unsubscribeEvent(p->bus, EV_TOPIC_GAMEPLAY, cb_order, p->peerCtx);
}

/* Callback that unsubscribes itself: it runs from the record it removes, so
 * the (callback, ctx) key matches by construction. */
static EventBus *g_self_bus;

static void cb_unsubscribes_self(void *ctx, const Event *event)
{
	(void)event;
	unsubscribeEvent(g_self_bus, EV_TOPIC_GAMEPLAY, cb_unsubscribes_self,
			 ctx);
}

/* Callback that subscribes another subscriber mid-dispatch. */
struct AddPeer {
	EventBus *bus;
	uint32_t topic;
	EventCallback callback;
	void *peerCtx;
};

static void cb_subscribes_peer(void *ctx, const Event *event)
{
	const struct AddPeer *p = ctx;

	(void)event;
	TEST_ASSERT_TRUE(subscribeEvent(p->bus, p->topic,
					p->callback, p->peerCtx));
}

/* Callback that publishes a follow-up event and then dispatches again
 * (reentrancy must be a no-op). */
static void cb_reenters(void *ctx, const Event *event)
{
	(void)ctx;
	(void)event;
	g_calls++;
	TEST_ASSERT_TRUE(publishEvent(g_self_bus, EV_TOPIC_ENGINE, 9u, NULL, 0));
	/* Running dispatch in flight: this nested call must return at once. */
	dispatchEvents(g_self_bus);
}

static void reset_globals(void)
{
	g_calls = 0;
	g_order_n = 0;
}

/* --- tests ------------------------------------------------------------ */

/* Deferred: publish queues, nothing is delivered until dispatchEvents, and
 * the queue drains (a second dispatch delivers nothing). */
static void test_publish_is_deferred_until_dispatch(void)
{
	EventBus *bus = createEventBus(8);

	TEST_ASSERT_NOT_NULL(bus);
	reset_globals();

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE, 1u, NULL, 0));
	TEST_ASSERT_EQUAL_INT(0, g_calls);

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ENGINE, cb_count, NULL));
	TEST_ASSERT_EQUAL_INT(0, g_calls);

	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	destroyEventBus(bus);
}

/* Copied payload ownership: the bus owns a private copy from publish until
 * after dispatch; the caller may scribble or free its own buffer at once. */
static void test_payload_is_copied(void)
{
	EventBus *bus = createEventBus(8);
	struct Expect expect = { "hello", "world", NULL, NULL, 0 };
	char stack_buf[8];
	char *heap_buf;

	TEST_ASSERT_NOT_NULL(bus);

	memcpy(stack_buf, "hello", 6);
	heap_buf = malloc(6);
	TEST_ASSERT_NOT_NULL(heap_buf);
	memcpy(heap_buf, "world", 6);
	expect.prov42 = stack_buf;
	expect.prov43 = heap_buf;

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_UI, 42u, stack_buf, 6));
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_UI, 43u, heap_buf, 6));

	/* Scribble the stack buffer and free the heap buffer before the bus
	 * has even seen a dispatch. */
	memset(stack_buf, '#', sizeof(stack_buf));
	free(heap_buf);
	heap_buf = NULL;

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_UI, cb_check_payload,
					&expect));

	dispatchEvents(bus);
	/* One subscriber, both events, each with its own private copy. */
	TEST_ASSERT_EQUAL_INT(2, expect.hits);

	destroyEventBus(bus);
}

/* Multiple subscribers: same-topic subscribers all run, in subscription
 * order; other-topic subscribers see nothing. */
static void test_multiple_subscribers_in_subscription_order(void)
{
	EventBus *bus = createEventBus(8);

	TEST_ASSERT_NOT_NULL(bus);
	reset_globals();

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_a));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_b));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_UI, cb_order, &id_c));

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 1u, NULL, 0));
	dispatchEvents(bus);

	TEST_ASSERT_EQUAL_INT(2, g_order_n);
	TEST_ASSERT_EQUAL_INT(0x0A, g_order[0]);
	TEST_ASSERT_EQUAL_INT(0x0B, g_order[1]);

	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(2, g_order_n);

	destroyEventBus(bus);
}

/* Unsubscribing a subscriber that has not run yet in the current dispatch
 * prevents its call in that dispatch (and every later one); subscribers
 * after it are unaffected. */
static void test_unsubscribe_during_callback_prevents_later_call(void)
{
	EventBus *bus = createEventBus(8);
	struct UnsubPeer peer = { NULL, &id_b };

	TEST_ASSERT_NOT_NULL(bus);
	peer.bus = bus;
	reset_globals();

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY,
					cb_unsubscribes_peer, &peer));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_b));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_c));

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 1u, NULL, 0));
	dispatchEvents(bus);

	/* B was unsubscribed before its turn: only C ran. */
	TEST_ASSERT_EQUAL_INT(1, g_order_n);
	TEST_ASSERT_EQUAL_INT(0x0C, g_order[0]);

	/* B is gone for good. */
	reset_globals();
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 1u, NULL, 0));
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_order_n);
	TEST_ASSERT_EQUAL_INT(0x0C, g_order[0]);

	destroyEventBus(bus);
}

/* A subscriber that unsubscribes ITSELF mid-callback is safe, and every
 * later subscriber in the list still runs in that same dispatch (guards the
 * index-shift hole). */
static void test_self_unsubscribe_during_callback_is_safe(void)
{
	EventBus *bus = createEventBus(8);

	TEST_ASSERT_NOT_NULL(bus);
	reset_globals();
	g_self_bus = bus;

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY,
					cb_unsubscribes_self, &id_a));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_b));
	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_GAMEPLAY, cb_order, &id_c));

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 1u, NULL, 0));
	dispatchEvents(bus);

	/* A ran (its callback doesn't record); B and C both ran after it. */
	TEST_ASSERT_EQUAL_INT(2, g_order_n);
	TEST_ASSERT_EQUAL_INT(0x0B, g_order[0]);
	TEST_ASSERT_EQUAL_INT(0x0C, g_order[1]);

	/* A does not re-fire on later dispatches: it really unsubscribed. */
	reset_globals();
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_GAMEPLAY, 1u, NULL, 0));
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(2, g_order_n);

	destroyEventBus(bus);
}

/* Bounded queue: capacity bounds queue length; surplus publishes fail and
 * leave the queue untouched. */
static void test_queue_overflow_rejects_and_delivers_prefix(void)
{
	EventBus *small = createEventBus(2);
	const char msg[] = "p2";

	TEST_ASSERT_NOT_NULL(small);
	reset_globals();

	TEST_ASSERT_TRUE(publishEvent(small, EV_TOPIC_ENGINE, 1u, NULL, 0));
	TEST_ASSERT_TRUE(publishEvent(small, EV_TOPIC_ENGINE, 2u, msg, sizeof(msg)));
	TEST_ASSERT_FALSE(publishEvent(small, EV_TOPIC_ENGINE, 3u, NULL, 0));
	TEST_ASSERT_FALSE(publishEvent(small, EV_TOPIC_ENGINE, 4u, msg, sizeof(msg)));

	TEST_ASSERT_TRUE(subscribeEvent(small, EV_TOPIC_ENGINE, cb_count, NULL));
	dispatchEvents(small);
	TEST_ASSERT_EQUAL_INT(2, g_calls);

	/* After the drain there is room again. */
	TEST_ASSERT_TRUE(publishEvent(small, EV_TOPIC_ENGINE, 5u, NULL, 0));
	reset_globals();
	dispatchEvents(small);
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	destroyEventBus(small);
}

/* A subscriber subscribed from inside a callback is skipped in the running
 * dispatch and first called on the next one. */
static void test_subscribe_during_callback_waits_for_next_dispatch(void)
{
	EventBus *bus = createEventBus(8);
	struct AddPeer peer;

	TEST_ASSERT_NOT_NULL(bus);
	peer.bus = bus;
	peer.topic = EV_TOPIC_ENGINE;
	peer.callback = cb_count;
	peer.peerCtx = NULL;
	reset_globals();

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ENGINE,
					cb_subscribes_peer, &peer));
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE, 1u, NULL, 0));

	/* A fires and adds B; B must not be called for this dispatch. */
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(0, g_calls);

	/* B is live from the next dispatch on; remove the adder so the tail
	 * of this test counts only B. */
	TEST_ASSERT_TRUE(unsubscribeEvent(bus, EV_TOPIC_ENGINE,
					  cb_subscribes_peer, &peer));
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE, 2u, NULL, 0));
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	/* B stays live afterward. */
	reset_globals();
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE, 3u, NULL, 0));
	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	destroyEventBus(bus);
}

/* dispatchEvents is not reentrant: a nested call returns immediately
 * (no recursion, no state change), and an event published by the callback
 * is delivered on the next top-level dispatch. */
static void test_reentrant_dispatch_is_noop(void)
{
	EventBus *bus = createEventBus(8);

	TEST_ASSERT_NOT_NULL(bus);
	reset_globals();
	g_self_bus = bus;

	TEST_ASSERT_TRUE(subscribeEvent(bus, EV_TOPIC_ENGINE, cb_reenters, NULL));
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ENGINE, 1u, NULL, 0));

	dispatchEvents(bus);
	/* One top-level delivery; the nested dispatch ran nothing, and the
	 * callback's own publish is still queued. */
	TEST_ASSERT_EQUAL_INT(1, g_calls);

	dispatchEvents(bus);
	TEST_ASSERT_EQUAL_INT(2, g_calls);

	destroyEventBus(bus);
}

/* Null and edge arguments. */
static void test_null_and_edge_arguments(void)
{
	EventBus *bus = createEventBus(8);

	TEST_ASSERT_NOT_NULL(bus);
	reset_globals();

	/* A bus with no queue capacity is not a bus. */
	destroyEventBus(NULL);
	TEST_ASSERT_NULL(createEventBus(0));

	TEST_ASSERT_FALSE(subscribeEvent(NULL, EV_TOPIC_UI, cb_order, &id_a));
	TEST_ASSERT_FALSE(publishEvent(NULL, EV_TOPIC_UI, 1u, NULL, 0));
	TEST_ASSERT_FALSE(unsubscribeEvent(NULL, EV_TOPIC_UI, cb_order, &id_a));
	dispatchEvents(NULL);	/* must be a harmless no-op */

	TEST_ASSERT_FALSE(subscribeEvent(bus, EV_TOPIC_UI, NULL, NULL));
	TEST_ASSERT_FALSE(unsubscribeEvent(bus, EV_TOPIC_UI, cb_order, &id_a));

	/* An accepted publish with a NULL zero-length payload is fine. */
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_UI, 1u, NULL, 0));
	dispatchEvents(bus);	/* no subscriber: single drain, no crash */

	destroyEventBus(bus);
}

/* Destroy frees undelivered payloads (leak-checked under the sanitizer run:
 * two queued payload copies must not outlive the bus). */
static void test_destroy_frees_undelivered_payloads(void)
{
	EventBus *bus = createEventBus(8);
	char buf[8];
	void *heap_buf;

	TEST_ASSERT_NOT_NULL(bus);

	memcpy(buf, "alpha", 6);
	heap_buf = malloc(6);
	TEST_ASSERT_NOT_NULL(heap_buf);
	memcpy(heap_buf, "bravo", 6);

	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ACHIEVEMENT, 7u, buf, 6));
	TEST_ASSERT_TRUE(publishEvent(bus, EV_TOPIC_ACHIEVEMENT, 8u, heap_buf, 6));

	/* Only delivery frees delivered copies; destroy must own these. */
	destroyEventBus(bus);
	free(heap_buf);
}

/* --- runner ----------------------------------------------------------- */

/* Called by test_main.c (the one headless executable's runner). */
void run_test_events(void);

void run_test_events(void)
{
	RUN_TEST(test_publish_is_deferred_until_dispatch);
	RUN_TEST(test_payload_is_copied);
	RUN_TEST(test_multiple_subscribers_in_subscription_order);
	RUN_TEST(test_unsubscribe_during_callback_prevents_later_call);
	RUN_TEST(test_self_unsubscribe_during_callback_is_safe);
	RUN_TEST(test_queue_overflow_rejects_and_delivers_prefix);
	RUN_TEST(test_subscribe_during_callback_waits_for_next_dispatch);
	RUN_TEST(test_reentrant_dispatch_is_noop);
	RUN_TEST(test_null_and_edge_arguments);
	RUN_TEST(test_destroy_frees_undelivered_payloads);
}
