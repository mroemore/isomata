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

typedef struct TdlImage {
	int x;
	int y;
	int w;
	int h;
	int icon;	/* recorded UiIcon value */
	uint32_t rgba;
} TdlImage;

typedef struct TdlText {
	int x;
	int y;
	char text[TDL_TEXT_MAX];
	uint32_t rgba;
	/* Snapshot of the style fields the draw call passed, so a test can
	 * assert style passthrough (the label/button/menu all forward their
	 * own TextStyle). pixelSize is -1 when style was NULL. */
	int stylePixelSize;
	const UiFont *styleFont;
	int (*styleMeasureFn)(void *ctx, const char *text, int *w, int *h);
} TdlText;

typedef struct TestDrawLog {
	TdlFill fills[TDL_MAX_CALLS];
	int nFills;
	TdlText texts[TDL_MAX_CALLS];
	int nTexts;
	TdlImage images[TDL_MAX_CALLS];
	int nImages;
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
	TdlText *slot;

	if (log == NULL || log->nTexts >= TDL_MAX_CALLS)
		return;
	slot = &log->texts[log->nTexts];
	slot->x = x;
	slot->y = y;
	snprintf(slot->text, TDL_TEXT_MAX, "%s", text != NULL ? text : "(null)");
	slot->rgba = rgba;
	if (style != NULL) {
		slot->stylePixelSize = style->pixelSize;
		slot->styleFont = style->font;
		slot->styleMeasureFn = style->measure.fn;
	} else {
		slot->stylePixelSize = -1;
		slot->styleFont = NULL;
		slot->styleMeasureFn = NULL;
	}
	log->nTexts++;
}

static inline void tdl_drawImage(UiDrawCtx *ctx, int x, int y, int w, int h,
				 UiIcon icon, uint32_t tint)
{
	TestDrawLog *log = tdlLog(ctx);
	TdlImage *slot;

	if (log == NULL || log->nImages >= TDL_MAX_CALLS)
		return;
	slot = &log->images[log->nImages];
	slot->x = x;
	slot->y = y;
	slot->w = w;
	slot->h = h;
	slot->icon = (int)icon;
	slot->rgba = tint;
	log->nImages++;
}

static const UiDrawCtxVt tdlVt = {
	tdl_fillRect,
	tdl_drawText,
	tdl_drawImage,
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
