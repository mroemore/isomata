#ifndef ISOMATA_AUDIO_AUDIO_H
#define ISOMATA_AUDIO_AUDIO_H

/*
 * SDL3 audio tier: a small fire-and-forget sound player. This is the ONLY
 * audio file that includes SDL; it is compiled into the app target
 * (engine_sources), never into isomata_pure, so the headless suite stays
 * device-free.
 *
 * Design (controller ruling #2):
 * - One SDL_AudioStream per voice (AUDIO_VOICE_COUNT), all bound to one
 *   default playback device. SDL mixes the bound streams; there is no custom
 *   mixer callback.
 * - The three sounds are loaded once at init (SDL_LoadWAV) into
 *   bus-owned buffers in the WAV source format (S16 mono 44100); each voice
 *   stream converts that format to the device's actual format. Sounds whose
 *   WAV spec differs are converted at load time.
 * - audioPlay picks the first voice with no queued data, sets its gain from
 *   the master gain and queues the sound. When every voice is busy the play
 *   is DROPPED silently (one log line), and a play request for an
 *   out-of-range/unloaded sound is a no-op.
 * - Init is TOLERANT: if SDL audio cannot start, the device cannot open, a
 *   WAV cannot load, or a stream cannot be created, audioCreate logs a clear
 *   message, releases whatever it made and returns NULL. A NULL Audio is a
 *   disabled player: every entry point tolerates it, so the app and the
 *   smoke run work without an audio device.
 *
 * Event vocabulary (topic EV_TOPIC_AUDIO, per-topic types start at 0):
 * - EV_AUDIO_PLAY: payload AudioPlayRequest (the SoundId to play). The bus
 *   copies the request at publish time, so the caller may reuse its buffer.
 * - Audio also subscribes to EV_TOPIC_ACHIEVEMENT: EV_ACHIEVEMENT_UNLOCKED
 *   (achievement.h) plays SOUND_ACHIEVEMENT.
 */

#include "events.h"

#include <stdbool.h>

/* Event type (topic EV_TOPIC_AUDIO). */
enum {
	EV_AUDIO_PLAY = 0,
};

/* The committed sounds under assets/audio/. Order is part of the enum. */
typedef enum SoundId {
	SOUND_MENU = 0,
	SOUND_ROTATE,
	SOUND_ACHIEVEMENT,
	SOUND_COUNT,
} SoundId;

/* Payload for EV_AUDIO_PLAY: which sound to play. */
typedef struct AudioPlayRequest {
	int sound;	/* a SoundId */
} AudioPlayRequest;

#define AUDIO_VOICE_COUNT 8

typedef struct Audio Audio;

/* Tolerant init (see the note above). NULL means "disabled". */
Audio *audioCreate(void);

/* Unbind/free every voice, close the device, free the sound buffers and
 * unsubscribe from the bus. Tolerates NULL. */
void audioDestroy(Audio *audio);

/* Queue `id` on a free voice, or drop it when none is free. Tolerates a NULL
 * or disabled Audio. */
void audioPlay(Audio *audio, SoundId id);

/* Subscribe the Audio to EV_TOPIC_AUDIO (play requests) and
 * EV_TOPIC_ACHIEVEMENT (the unlock jingle). NULL audio or bus = no-op. */
void audioSubscribe(Audio *audio, EventBus *bus);

/* Master gain, clamped to [0, 1]; applied to every voice. NULL = no-op. */
void audioSetGain(Audio *audio, float gain);

/* Current master gain; 0 for a NULL Audio. */
float audioGain(const Audio *audio);

/* True when the device and all sounds are ready. */
bool audioReady(const Audio *audio);

#endif /* ISOMATA_AUDIO_AUDIO_H */
