/*
 * Shared recording draw context for the ui test twins (NOT a twin itself;
 * never listed in meson). The pinned UiDrawCtx is just { vt } with no
 * private-state slot, so the recorder tracks the ONE TestDrawLog that is
 * currently active via a file-static pointer: call tdlUse(log) at the
 * start of a test's draws, assert, then tdlReset(log)/tdlUse for the
 * next. Tests run strictly sequentially, so one active log at a time is
 * always correct.
 */

#ifndef UI_TEST_UTIL_H
#define UI_TEST_UTIL_H

#include "unity.h"

#include "ui/element.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TDL_MAX_CALLS 16
#define TDL_TEXT_MAX 48

typedef struct TdlFill {
	int x;
	int y;
	int w;
	int h;
	uint32_t rgba;
} TdlFill;

typedef struct TdlText {
	int x;
	int y;
	char text[TDL_TEXT_MAX];
	uint32_t rgba;
} TdlText;

typedef struct TestDrawLog {
	TdlFill fills[TDL_MAX_CALLS];
	int nFills;
	TdlText texts[TDL_MAX_CALLS];
	int nTexts;
} TestDrawLog;

static TestDrawLog *tdl_active;

static inline TestDrawLog *tdlLog(UiDrawCtx *ctx)
{
	(void)ctx;
	return tdl_active;
}

static inline void tdl_fillRect(UiDrawCtx *ctx, int x, int y, int w, int h, uint32_t rgba)
{
	TestDrawLog *log = tdlLog(ctx);

	if (log == NULL || log->nFills >= TDL_MAX_CALLS)
		return;
	log->fills[log->nFills].x = x;
	log->fills[log->nFills].y = y;
	log->fills[log->nFills].w = w;
	log->fills[log->nFills].h = h;
	log->fills[log->nFills].rgba = rgba;
	log->nFills++;
}

static inline void tdl_drawText(UiDrawCtx *ctx, int x, int y, const char *text,
				const struct TextStyle *style, uint32_t rgba)
{
	TestDrawLog *log = tdlLog(ctx);

	(void)style;
	if (log == NULL || log->nTexts >= TDL_MAX_CALLS)
		return;
	log->texts[log->nTexts].x = x;
	log->texts[log->nTexts].y = y;
	snprintf(log->texts[log->nTexts].text, TDL_TEXT_MAX, "%s", text != NULL ? text : "(null)");
	log->texts[log->nTexts].rgba = rgba;
	log->nTexts++;
}

static const UiDrawCtxVt tdlVt = {
	tdl_fillRect,
	tdl_drawText,
};

/* Activate a log for the upcoming draws and wrap it in a fresh ctx. */
static inline UiDrawCtx tdlCtx(TestDrawLog *log)
{
	UiDrawCtx ctx = { &tdlVt };

	tdl_active = log;
	return ctx;
}

/* Clear counts (contents may stay partially stale; the counters gate). */
static inline void tdlReset(TestDrawLog *log)
{
	memset(log, 0, sizeof(*log));
}

#endif /* UI_TEST_UTIL_H */
