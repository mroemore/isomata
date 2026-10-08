/*
 * SDL3 audio tier (see audio.h). One default playback device with
 * AUDIO_VOICE_COUNT bound streams; SDL does the mixing. No custom callback.
 */

#include "audio/audio.h"

#include "achievement.h"
#include "platform/platform.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

/* The committed WAVs are 16-bit mono 44100; voices convert this source
 * format to the device's actual format. */
#define AUDIO_SRC_FORMAT SDL_AUDIO_S16
#define AUDIO_SRC_CHANNELS 1
#define AUDIO_SRC_RATE 44100

static void audioOnEvent(void *ctx, const Event *event);

struct Audio {
	SDL_AudioDeviceID device;
	SDL_AudioStream *voices[AUDIO_VOICE_COUNT];
	Uint8 *data[SOUND_COUNT];
	Uint32 len[SOUND_COUNT];
	float gain;
	EventBus *bus;
};

static const char *soundAsset(SoundId id)
{
	switch (id) {
	case SOUND_MENU:
		return "audio/menu.wav";
	case SOUND_ROTATE:
		return "audio/rotate.wav";
	case SOUND_ACHIEVEMENT:
		return "audio/achievement.wav";
	default:
		return NULL;
	}
}

/* Load one WAV into the source format. Takes ownership of the SDL_LoadWAV
 * buffer (or the converted one). Returns false (with a log) on failure. */
static bool loadSound(Audio *a, SoundId id)
{
	const SDL_AudioSpec src = { AUDIO_SRC_FORMAT, AUDIO_SRC_CHANNELS,
				    AUDIO_SRC_RATE };
	const char *asset = soundAsset(id);
	char path[512];
	SDL_AudioSpec spec;
	Uint8 *data = NULL;
	Uint32 len = 0;

	if (asset == NULL ||
	    platformAssetPath(asset, path, sizeof(path)) == NULL) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "audio: could not resolve %s", asset ? asset : "?");
		return false;
	}
	if (!SDL_LoadWAV(path, &spec, &data, &len)) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
			     "audio: SDL_LoadWAV(%s) failed: %s", path,
			     SDL_GetError());
		return false;
	}
	if (spec.format != src.format || spec.channels != src.channels ||
	    spec.freq != src.freq) {
		Uint8 *conv = NULL;
		int convLen = 0;

		if (!SDL_ConvertAudioSamples(&spec, data, (int)len, &src, &conv,
					     &convLen)) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "audio: convert %s failed: %s", path,
				     SDL_GetError());
			SDL_free(data);
			return false;
		}
		SDL_free(data);
		data = conv;
		len = (Uint32)convLen;
	}
	a->data[id] = data;
	a->len[id] = len;
	return true;
}

Audio *audioCreate(void)
{
	SDL_AudioSpec deviceSpec;
	const SDL_AudioSpec src = { AUDIO_SRC_FORMAT, AUDIO_SRC_CHANNELS,
				    AUDIO_SRC_RATE };
	Audio *a;
	int i;

	a = calloc(1, sizeof(*a));
	if (a == NULL)
		return NULL;
	a->gain = 1.0f;

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		SDL_Log("audio: disabled (SDL audio init failed: %s)",
			SDL_GetError());
		free(a);
		return NULL;
	}

	for (i = 0; i < SOUND_COUNT; i++) {
		if (!loadSound(a, (SoundId)i)) {
			audioDestroy(a);
			return NULL;
		}
	}

	a->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
					NULL);
	if (a->device == 0) {
		SDL_Log("audio: disabled (no playback device: %s)",
			SDL_GetError());
		audioDestroy(a);
		return NULL;
	}
	if (!SDL_GetAudioDeviceFormat(a->device, &deviceSpec, NULL)) {
		SDL_Log("audio: disabled (device format unavailable: %s)",
			SDL_GetError());
		audioDestroy(a);
		return NULL;
	}

	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		SDL_AudioStream *stream =
			SDL_CreateAudioStream(&src, &deviceSpec);

		if (stream == NULL ||
		    !SDL_BindAudioStream(a->device, stream)) {
			SDL_Log("audio: disabled (voice %d: %s)", i,
				SDL_GetError());
			if (stream != NULL)
				SDL_DestroyAudioStream(stream);
			audioDestroy(a);
			return NULL;
		}
		a->voices[i] = stream;
	}

	SDL_Log("audio: ready (driver %s, %d voices, device %u, %d Hz %d ch)",
		SDL_GetCurrentAudioDriver(), AUDIO_VOICE_COUNT,
		(unsigned)a->device, deviceSpec.freq, deviceSpec.channels);
	return a;
}

void audioDestroy(Audio *a)
{
	int i;

	if (a == NULL)
		return;
	if (a->bus != NULL) {
		unsubscribeEvent(a->bus, EV_TOPIC_AUDIO, audioOnEvent, a);
		unsubscribeEvent(a->bus, EV_TOPIC_ACHIEVEMENT, audioOnEvent, a);
	}
	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		if (a->voices[i] != NULL)
			SDL_DestroyAudioStream(a->voices[i]);	/* unbinds */
	}
	if (a->device != 0)
		SDL_CloseAudioDevice(a->device);
	for (i = 0; i < SOUND_COUNT; i++)
		SDL_free(a->data[i]);
	if (SDL_WasInit(SDL_INIT_AUDIO))
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	free(a);
}

void audioPlay(Audio *a, SoundId id)
{
	int i;

	if (a == NULL || a->device == 0)
		return;
	if (id < 0 || id >= SOUND_COUNT || a->data[id] == NULL ||
	    a->len[id] == 0)
		return;
	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		SDL_AudioStream *stream = a->voices[i];

		if (stream != NULL && SDL_GetAudioStreamQueued(stream) == 0) {
			SDL_SetAudioStreamGain(stream, a->gain);
			if (!SDL_PutAudioStreamData(stream, a->data[id],
						   (int)a->len[id]))
				SDL_Log("audio: put sound %d failed: %s",
					(int)id, SDL_GetError());
			else
				SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
					     "audio: play sound %d on voice %d",
					     (int)id, i);
			return;
		}
	}
	SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
		     "audio: all %d voices busy, dropped sound %d",
		     AUDIO_VOICE_COUNT, (int)id);
}

void audioSetGain(Audio *a, float gain)
{
	int i;

	if (a == NULL)
		return;
	if (gain < 0.0f)
		gain = 0.0f;
	if (gain > 1.0f)
		gain = 1.0f;
	a->gain = gain;
	for (i = 0; i < AUDIO_VOICE_COUNT; i++) {
		if (a->voices[i] != NULL)
			SDL_SetAudioStreamGain(a->voices[i], gain);
	}
}

float audioGain(const Audio *a)
{
	return a != NULL ? a->gain : 0.0f;
}

bool audioReady(const Audio *a)
{
	return a != NULL && a->device != 0;
}

static void audioOnEvent(void *ctx, const Event *event)
{
	Audio *a = ctx;

	if (event->topic == EV_TOPIC_AUDIO &&
	    event->type == EV_AUDIO_PLAY) {
		if (event->payload != NULL &&
		    event->payloadSize >= sizeof(AudioPlayRequest))
			audioPlay(a,
				  (SoundId)((const AudioPlayRequest *)
						    event->payload)
					  ->sound);
	} else if (event->topic == EV_TOPIC_ACHIEVEMENT &&
		   event->type == EV_ACHIEVEMENT_UNLOCKED) {
		audioPlay(a, SOUND_ACHIEVEMENT);
	}
}

void audioSubscribe(Audio *a, EventBus *bus)
{
	if (a == NULL || bus == NULL)
		return;
	a->bus = bus;
	subscribeEvent(bus, EV_TOPIC_AUDIO, audioOnEvent, a);
	subscribeEvent(bus, EV_TOPIC_ACHIEVEMENT, audioOnEvent, a);
}
