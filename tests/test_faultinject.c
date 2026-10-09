/*
 * Fault-injection target (CTOL rung 4): allocation-failure paths.
 *
 * Links tests/support/faultinject.c and is built with the --wrap recipe in
 * tests/meson.build, so every malloc/calloc/realloc issued by the linked
 * first-party objects routes through the shims. Each test sweeps the failure
 * index (fail the n-th allocation, then the (n+1)-th, ...) and asserts the
 * code under test either succeeds or fails cleanly with NO leaked
 * allocation. This closes the allocation-failure arms the coverage floors
 * previously excused (events.c, scene.c, input.c, drawlist.c, achievement.c).
 *
 * Uses the CTOL micro-harness (tests/harness.h) with
 * -DISOMATA_HARNESS_FAULTINJECT, which is exactly the opt-in that harness.h
 * documents; the assertion-returns contract is what makes the loop-until-clean
 * sweep possible.
 */

#include "harness.h"

#include "achievement.h"
#include "events.h"
#include "input/input.h"
#include "render/drawlist.h"
#include "render/lightgrid.h"
#include "render/voxmap.h"
#include "scene.h"

#include <string.h>

#define FAIL_SWEEP_LIMIT 8	/* > every target's allocation count */

static void noopCallback(void *ctx, const Event *event)
{
	(void)ctx;
	(void)event;
}

static const SceneVt dummyVt = {
	.init = NULL,
	.update = NULL,
	.draw = NULL,
	.unload = NULL,
	.size = 0,
	.name = "dummy",
};

/* createEventBus: fail each allocation index; a NULL result must mean an
 * injected failure happened and left nothing allocated. */
static int test_event_bus_alloc_failures(void)
{
	long n;

	for (n = 0; n < FAIL_SWEEP_LIMIT; n++) {
		EventBus *bus;

		fi_reset();
		fi_fail_after(n);
		bus = createEventBus(8);
		if (bus != NULL) {
			destroyEventBus(bus);
		} else {
			ASSERT_TRUE(fi_failures() > 0);
		}
		fi_fail_after(-1);
		ASSERT_EQ_INT(0, fi_live());
	}
	return 0;
}

/* publishEvent: the payload copy malloc is the only allocation; when it
 * fails the event must not be queued (dispatch delivers nothing) and the bus
 * must not leak. */
static int test_publish_payload_alloc_failure(void)
{
	EventBus *bus;
	int payload = 7;

	fi_reset();
	bus = createEventBus(4);
	ASSERT_NOT_NULL(bus);
	ASSERT_TRUE(subscribeEvent(bus, 3, noopCallback, NULL));

	fi_reset();
	fi_fail_after(0);
	ASSERT_FALSE(publishEvent(bus, 3, 1, &payload, sizeof(payload)));
	ASSERT_TRUE(fi_failures() >= 1);
	fi_fail_after(-1);

	dispatchEvents(bus);	/* nothing queued: no callback, no crash */
	destroyEventBus(bus);
	ASSERT_EQ_INT(0, fi_live());
	return 0;
}

/* subscribeEvent: the record-array realloc fails on the first subscribe and
 * again on a growth; both must return false without leaking. */
static int test_subscribe_realloc_failures(void)
{
	EventBus *bus;

	fi_reset();
	bus = createEventBus(4);
	ASSERT_NOT_NULL(bus);

	fi_reset();
	fi_fail_after(0);	/* first realloc: subs 0 -> 4 */
	ASSERT_FALSE(subscribeEvent(bus, 1, noopCallback, NULL));
	fi_fail_after(-1);

	ASSERT_TRUE(subscribeEvent(bus, 1, noopCallback, NULL));
	ASSERT_TRUE(subscribeEvent(bus, 2, noopCallback, NULL));
	ASSERT_TRUE(subscribeEvent(bus, 3, noopCallback, NULL));
	ASSERT_TRUE(subscribeEvent(bus, 4, noopCallback, NULL));

	fi_reset();
	fi_fail_after(0);	/* growth realloc: subs 4 -> 8 */
	ASSERT_FALSE(subscribeEvent(bus, 5, noopCallback, NULL));
	fi_fail_after(-1);

	destroyEventBus(bus);
	ASSERT_EQ_INT(0, fi_live());
	return 0;
}

/* createInput: a failed calloc returns NULL and leaks nothing. */
static int test_input_alloc_failure(void)
{
	Input *input;

	fi_reset();
	fi_fail_after(0);
	input = createInput();
	ASSERT_NULL(input);
	ASSERT_TRUE(fi_failures() >= 1);
	fi_fail_after(-1);
	ASSERT_EQ_INT(0, fi_live());

	input = createInput();
	ASSERT_NOT_NULL(input);
	destroyInput(input);
	ASSERT_EQ_INT(0, fi_live());
	return 0;
}

/* createScene and createSceneStack: every allocation index. */
static int test_scene_alloc_failures(void)
{
	long n;

	for (n = 0; n < FAIL_SWEEP_LIMIT; n++) {
		Scene *scene;
		SceneStack *stack;

		fi_reset();
		fi_fail_after(n);
		scene = createScene(&dummyVt);
		if (scene != NULL)
			destroyScene(scene);
		else
			ASSERT_TRUE(fi_failures() > 0);
		fi_fail_after(-1);
		ASSERT_EQ_INT(0, fi_live());

		fi_reset();
		fi_fail_after(n);
		stack = createSceneStack(4);
		if (stack != NULL)
			destroySceneStack(stack);
		else
			ASSERT_TRUE(fi_failures() > 0);
		fi_fail_after(-1);
		ASSERT_EQ_INT(0, fi_live());
	}
	return 0;
}

/* initDrawList: a failed item-array malloc leaves an empty, never-appendable
 * list that still destroys cleanly. */
static int test_drawlist_alloc_failure(void)
{
	DrawList list;
	DrawItem item;

	memset(&item, 0, sizeof(item));

	fi_reset();
	fi_fail_after(0);
	initDrawList(&list, 16);
	ASSERT_EQ_UINT(0, drawListCount(&list));
	ASSERT_FALSE(appendDrawItem(&list, &item));
	fi_fail_after(-1);
	destroyDrawList(&list);
	ASSERT_EQ_INT(0, fi_live());

	initDrawList(&list, 16);
	ASSERT_TRUE(appendDrawItem(&list, &item));
	ASSERT_EQ_UINT(1, drawListCount(&list));
	destroyDrawList(&list);
	ASSERT_EQ_INT(0, fi_live());
	return 0;
}

/* createAchievementSystem: allocation failure or failed subscribe returns
 * NULL and releases the partial state (no leak, no stray subscription). */
static int test_achievement_alloc_failures(void)
{
	long n;

	for (n = 0; n < FAIL_SWEEP_LIMIT; n++) {
		EventBus *bus;
		AchievementSystem *system;

		fi_reset();
		bus = createEventBus(4);
		ASSERT_NOT_NULL(bus);

		fi_reset();
		fi_fail_after(n);
		system = createAchievementSystem(bus);
		if (system != NULL)
			destroyAchievementSystem(system);
		else
			ASSERT_TRUE(fi_failures() > 0);
		fi_fail_after(-1);

		destroyEventBus(bus);
		ASSERT_EQ_INT(0, fi_live());
	}
	return 0;
}

/* parseVoxmapText: the map struct, cell array and material array are the only
 * allocations; each failure index must yield NULL with nothing leaked. */
static int test_voxmap_alloc_failures(void)
{
	long n;

	for (n = 0; n < FAIL_SWEEP_LIMIT; n++) {
		Voxmap *map;

		fi_reset();
		fi_fail_after(n);
		map = parseVoxmapText("12\n34\n", 6, NULL);
		if (map != NULL)
			destroyVoxmap(map);
		else
			ASSERT_TRUE(fi_failures() > 0);
		fi_fail_after(-1);
		ASSERT_EQ_INT(0, fi_live());
	}
	return 0;
}

/* lightGridCreate: the struct, the sky/block/solid planes and the two queue
 * arrays are the only allocations; each failure index must yield NULL with
 * nothing leaked. */
static int test_lightgrid_alloc_failures(void)
{
	long n;

	for (n = 0; n < FAIL_SWEEP_LIMIT; n++) {
		LightGrid *grid;

		fi_reset();
		fi_fail_after(n);
		grid = lightGridCreate(8, 8, 4);
		if (grid != NULL)
			destroyLightGrid(grid);
		else
			ASSERT_TRUE(fi_failures() > 0);
		fi_fail_after(-1);
		ASSERT_EQ_INT(0, fi_live());
	}
	return 0;
}

int main(void)
{
	int failed = 0;

	RUN(test_event_bus_alloc_failures);
	RUN(test_publish_payload_alloc_failure);
	RUN(test_subscribe_realloc_failures);
	RUN(test_input_alloc_failure);
	RUN(test_scene_alloc_failures);
	RUN(test_drawlist_alloc_failure);
	RUN(test_achievement_alloc_failures);
	RUN(test_voxmap_alloc_failures);
	RUN(test_lightgrid_alloc_failures);
	HARNESS_SUMMARY("faultinject");
}
