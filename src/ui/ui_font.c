/*
 * Real font loading + measurement over SDL3_ttf. See ui_font.h for the
 * invariant block. This translation unit is the SDL boundary of the ui
 * tree: it includes SDL headers, nothing else under src/ui/ does.
 */

#include "ui/ui_font.h"

#include <SDL3_ttf/SDL_ttf.h>

#include <stdlib.h>
#include <string.h>

struct UiFont {
	TTF_Font *ttf;
};

/* TextMeasure seam: 0 = success. Empty text is 0x0 (success); a NULL or
 * failed font is a measurement failure. */
static int fontMeasureFn(void *ctx, const char *text, int *w, int *h)
{
	UiFont *font = ctx;
	int width = 0;
	int height = 0;

	if (font == NULL || font->ttf == NULL)
		return 1;
	if (text == NULL || text[0] == '\0') {
		if (w != NULL)
			*w = 0;
		if (h != NULL)
			*h = 0;
		return 0;
	}
	if (!TTF_GetStringSize(font->ttf, text, strlen(text), &width, &height))
		return 1;
	if (w != NULL)
		*w = width;
	if (h != NULL)
		*h = height;
	return 0;
}

UiFont *uiLoadFont(const char *path, int pixelSize)
{
	UiFont *font;

	if (path == NULL || path[0] == '\0' || pixelSize <= 0)
		return NULL;

	/* Reference-counted: paired with the TTF_Quit in uiFreeFont. */
	if (!TTF_Init())
		return NULL;

	font = malloc(sizeof(*font));
	if (font == NULL) {
		TTF_Quit();
		return NULL;
	}
	font->ttf = TTF_OpenFont(path, (float)pixelSize);
	if (font->ttf == NULL) {
		free(font);
		TTF_Quit();
		return NULL;
	}
	return font;
}

void uiFreeFont(UiFont *font)
{
	if (font == NULL)
		return;
	if (font->ttf != NULL)
		TTF_CloseFont(font->ttf);
	free(font);
	TTF_Quit();
}

int uiFontHeight(const UiFont *font)
{
	if (font == NULL || font->ttf == NULL)
		return 0;
	return TTF_GetFontHeight(font->ttf);
}

TextMeasure uiFontMeasure(UiFont *font)
{
	TextMeasure measure = { NULL, NULL };

	if (font == NULL)
		return measure;
	measure.fn = fontMeasureFn;
	measure.ctx = font;
	return measure;
}
