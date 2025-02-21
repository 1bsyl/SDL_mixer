/*
  SDL_mixer:  An audio mixer library based on the SDL library
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/* This file supports playing MIDI files with timidity */

#ifdef MUSIC_MID_TIMIDITY

#include "music_timidity.h"

#include "timidity/timidity.h"


typedef struct
{
    int play_count;
    MidiSong *song;
    SDL_AudioStream *stream;
    void *buffer;
    Sint32 buffer_size;
    int volume;
} TIMIDITY_Music;


static int TIMIDITY_Seek(void *context, double position);
static void TIMIDITY_Delete(void *context);

/* Config file should contain any other directory that needs
 * to be added to the search path. The library adds the path
 * of the config file to its search path, too. */
#if defined(SDL_PLATFORM_WIN32)
# define TIMIDITY_CFG           "C:\\TIMIDITY\\TIMIDITY.CFG"
#else  /* unix: */
# define TIMIDITY_CFG_ETC       "/etc/timidity.cfg"
# define TIMIDITY_CFG_FREEPATS  "/etc/timidity/freepats.cfg"
#endif

static int TIMIDITY_Open(const SDL_AudioSpec *spec)
{
    const char *cfg;
    int rc = -1;

    (void) spec;

    cfg = SDL_getenv("TIMIDITY_CFG");
    if(!cfg) cfg = Mix_GetTimidityCfg();
    if (cfg) {
        return Timidity_Init(cfg); /* env or user override: no other tries */
    }
#if defined(TIMIDITY_CFG)
    if (rc < 0) rc = Timidity_Init(TIMIDITY_CFG);
#endif
#if defined(TIMIDITY_CFG_ETC)
    if (rc < 0) rc = Timidity_Init(TIMIDITY_CFG_ETC);
#endif
#if defined(TIMIDITY_CFG_FREEPATS)
    if (rc < 0) rc = Timidity_Init(TIMIDITY_CFG_FREEPATS);
#endif
    if (rc < 0) rc = Timidity_Init(NULL); /* library's default cfg. */

    return rc;
}

static void TIMIDITY_Close(void)
{
    Timidity_Exit();
}

void *TIMIDITY_CreateFromIO(SDL_IOStream *src, bool closeio)
{
    TIMIDITY_Music *music;
    SDL_AudioSpec spec;
    bool need_stream = false;

    music = (TIMIDITY_Music *)SDL_calloc(1, sizeof(*music));
    if (!music) {
        return NULL;
    }

    music->volume = MIX_MAX_VOLUME;

    SDL_memcpy(&spec, &music_spec, sizeof(spec));
    if (spec.channels > 2) {
        need_stream = true;
        spec.channels = 2;
    }
    music->song = Timidity_LoadSong(src, &spec);
    if (!music->song) {
        TIMIDITY_Delete(music);
        return NULL;
    }

    if (need_stream) {
        music->stream = SDL_CreateAudioStream(&spec, &music_spec);
        if (!music->stream) {
            TIMIDITY_Delete(music);
            return NULL;
        }

        music->buffer_size = 4096/*spec.samples*/ * (SDL_AUDIO_BITSIZE(spec.format) / 8) * spec.channels;
        music->buffer = SDL_malloc((size_t)music->buffer_size);
        if (!music->buffer) {
            TIMIDITY_Delete(music);
            return NULL;
        }
    }

    if (closeio) {
        SDL_CloseIO(src);
    }
    return music;
}

static void TIMIDITY_SetVolume(void *context, int volume)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    music->volume = volume;
    Timidity_SetVolume(music->song, volume);
}

static int TIMIDITY_GetVolume(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    return music->volume;
}

static int TIMIDITY_Play(void *context, int play_count)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    music->play_count = play_count;
    Timidity_Start(music->song);
    return TIMIDITY_Seek(music, 0.0);
}

static bool TIMIDITY_IsPlaying(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    return Timidity_IsActive(music->song);
}

static int TIMIDITY_GetSome(void *context, void *data, int bytes, bool *done)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    int filled, amount, expected;

    if (music->stream) {
        filled = SDL_GetAudioStreamData(music->stream, data, bytes);
        if (filled != 0) {
            return filled;
        }
    }

    if (!music->play_count) {
        /* All done */
        *done = true;
        return 0;
    }

    if (music->stream) {
        expected = music->buffer_size;
        amount = Timidity_PlaySome(music->song, music->buffer, music->buffer_size);
        if (!SDL_PutAudioStreamData(music->stream, music->buffer, amount)) {
            return -1;
        }
    } else {
        expected = bytes;
        amount = Timidity_PlaySome(music->song, data, bytes);
    }

    if (amount < expected) {
        if (music->play_count == 1) {
            /* We didn't consume anything and we're done */
            music->play_count = 0;
        } else {
            int play_count = -1;
            if (music->play_count > 0) {
                play_count = (music->play_count - 1);
            }
            if (TIMIDITY_Play(music, play_count) < 0) {
                return -1;
            }
        }
    }
    if (music->stream) {
        /* We'll pick it up from the stream next time around */
        return 0;
    } else {
        /* We wrote output data */
        return amount;
    }
}

static int TIMIDITY_GetAudio(void *context, void *data, int bytes)
{
    return music_pcm_getaudio(context, data, bytes, MIX_MAX_VOLUME, TIMIDITY_GetSome);
}

static int TIMIDITY_Seek(void *context, double position)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    Timidity_Seek(music->song, (Uint32)(position * 1000));
    return 0;
}

static double TIMIDITY_Tell(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    return Timidity_GetSongTime(music->song) / 1000.0;
}

static double TIMIDITY_Duration(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    return Timidity_GetSongLength(music->song) / 1000.0;
}

static void TIMIDITY_Delete(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;

    if (music->song) {
        Timidity_FreeSong(music->song);
    }
    if (music->stream) {
        SDL_DestroyAudioStream(music->stream);
    }
    if (music->buffer) {
        SDL_free(music->buffer);
    }
    SDL_free(music);
}

static void TIMIDITY_Stop(void *context)
{
    TIMIDITY_Music *music = (TIMIDITY_Music *)context;
    Timidity_Stop(music->song);
}

Mix_MusicInterface Mix_MusicInterface_TIMIDITY =
{
    "TIMIDITY",
    MIX_MUSIC_TIMIDITY,
    MUS_MID,
    false,
    false,

    NULL,   /* Load */
    TIMIDITY_Open,
    TIMIDITY_CreateFromIO,
    NULL,   /* CreateFromFile */
    TIMIDITY_SetVolume,
    TIMIDITY_GetVolume,
    TIMIDITY_Play,
    TIMIDITY_IsPlaying,
    TIMIDITY_GetAudio,
    NULL,   /* Jump */
    TIMIDITY_Seek,
    TIMIDITY_Tell,
    TIMIDITY_Duration,
    NULL,   /* LoopStart */
    NULL,   /* LoopEnd */
    NULL,   /* LoopLength */
    NULL,   /* GetMetaTag */
    NULL,   /* GetNumTracks */
    NULL,   /* StartTrack */
    NULL,   /* Pause */
    NULL,   /* Resume */
    TIMIDITY_Stop,
    TIMIDITY_Delete,
    TIMIDITY_Close,
    NULL    /* Unload */
};

#endif /* MUSIC_MID_TIMIDITY */

/* vi: set ts=4 sw=4 expandtab: */
