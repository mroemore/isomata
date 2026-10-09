/*
 * PNG slice map directory loader (see map_loader.h). SDL-tier glue: the only
 * place a map directory touches the filesystem / SDL image decoding. The pure
 * work (natural sort, legend parse, assembly) is delegated to mapsource.c.
 */

#include "render/map_loader.h"

#include "render/mapsource.h"

#include <SDL3/SDL.h>

#include <stdlib.h>
#include <string.h>

#define MAP_LOADER_PATH_MAX 512
#define MAP_LOADER_NAME_MAX 256

/* Collected *.png entry names (bounded by the slice cap). */
typedef struct NameList {
	char (*names)[MAP_LOADER_NAME_MAX];
	int count;
	int capacity;
	bool overflow;
} NameList;

static SDL_EnumerationResult SDLCALL collectPng(void *userdata,
						const char *dirname,
						const char *fname)
{
	NameList *list = userdata;
	size_t len = strlen(fname);

	(void)dirname;
	if (len < 4 || SDL_strcasecmp(fname + len - 4, ".png") != 0)
		return SDL_ENUM_CONTINUE;
	if (list->count >= list->capacity) {
		list->overflow = true;
		return SDL_ENUM_CONTINUE;
	}
	SDL_strlcpy(list->names[list->count], fname, MAP_LOADER_NAME_MAX);
	list->count++;
	return SDL_ENUM_CONTINUE;
}

/* qsort comparator over the fixed-size name slots. */
static int compareNames(const void *a, const void *b)
{
	return mapSourceNaturalCompare((const char *)a, (const char *)b);
}

static const char *joinPath(char *buffer, size_t size, const char *dir,
			    const char *name)
{
	int written = SDL_snprintf(buffer, size, "%s/%s", dir, name);

	if (written < 0 || (size_t)written >= size)
		return NULL;
	return buffer;
}

Voxmap *loadVoxmapDirectory(const char *dir, const MaterialTable *materials,
			    int *outSlices)
{
	NameList list;
	char path[MAP_LOADER_PATH_MAX];
	void *legendText = NULL;
	size_t legendSize = 0;
	MapSourceLegend legend;
	MapSourceImage *images = NULL;
	SDL_Surface **surfaces = NULL;
	Voxmap *map = NULL;
	int i;

	if (outSlices != NULL)
		*outSlices = 0;
	if (dir == NULL)
		return NULL;

	list.capacity = VOXMAP_MAX_DIM;
	list.count = 0;
	list.overflow = false;
	list.names = calloc((size_t)list.capacity, sizeof(*list.names));
	if (list.names == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: out of memory");
		return NULL;
	}
	if (!SDL_EnumerateDirectory(dir, collectPng, &list)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: cannot enumerate '%s': %s", dir,
			     SDL_GetError());
		free(list.names);
		return NULL;
	}
	if (list.count == 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: no *.png slices under '%s'", dir);
		free(list.names);
		return NULL;
	}
	if (list.overflow) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: more than %d slices under '%s'",
			     VOXMAP_MAX_DIM, dir);
		free(list.names);
		return NULL;
	}
	qsort(list.names, (size_t)list.count, sizeof(*list.names), compareNames);

	if (joinPath(path, sizeof(path), dir, "legend.txt") == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: path too long for '%s'", dir);
		free(list.names);
		return NULL;
	}
	legendText = SDL_LoadFile(path, &legendSize);
	if (legendText == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: cannot read '%s': %s", path,
			     SDL_GetError());
		free(list.names);
		return NULL;
	}
	if (!mapSourceLegendParse(legendText, legendSize, materials, &legend)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: legend '%s' unparsable", path);
		SDL_free(legendText);
		free(list.names);
		return NULL;
	}
	SDL_free(legendText);

	images = calloc((size_t)list.count, sizeof(*images));
	surfaces = calloc((size_t)list.count, sizeof(*surfaces));
	if (images == NULL || surfaces == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: out of memory");
		goto done;
	}

	for (i = 0; i < list.count; i++) {
		SDL_Surface *raw;
		SDL_Surface *converted;

		if (joinPath(path, sizeof(path), dir, list.names[i]) == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "map_loader: path too long for slice %d", i);
			goto done;
		}
		raw = SDL_LoadPNG(path);
		if (raw == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "map_loader: slice '%s' failed to load: %s",
				     path, SDL_GetError());
			goto done;
		}
		converted = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
		SDL_DestroySurface(raw);
		if (converted == NULL) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "map_loader: slice '%s' convert failed: %s",
				     path, SDL_GetError());
			goto done;
		}
		surfaces[i] = converted;
		images[i].width = converted->w;
		images[i].height = converted->h;
		images[i].rgba = converted->pixels;
		images[i].pitch = (size_t)converted->pitch;
	}

	map = mapSourceAssembleSlices(images, list.count, &legend, dir, NULL);
	if (map == NULL)
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "map_loader: assembly failed for '%s'", dir);
	else if (outSlices != NULL)
		*outSlices = list.count;

done:
	if (surfaces != NULL) {
		for (i = 0; i < list.count; i++)
			if (surfaces[i] != NULL)
				SDL_DestroySurface(surfaces[i]);
	}
	free(surfaces);
	free(images);
	free(list.names);
	return map;
}
