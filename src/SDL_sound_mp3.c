/**
 * SDL_sound; An abstract sound format decoding API.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

/*
 * MP3 decoder for SDL_sound.
 *
 * Uses dr_mp3, a public domain, single-header library.
 *
 * dr_mp3 is here: https://github.com/mackron/dr_libs/
 */

#define __SDL_SOUND_INTERNAL__
#include "SDL_sound_internal.h"

#if SOUND_SUPPORTS_MP3

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO 1
#define DR_MP3_FLOAT_OUTPUT 1
#define DRMP3_ASSERT(x) SDL_assert((x))
#define DRMP3_MALLOC(sz) SDL_malloc((sz))
#define DRMP3_REALLOC(p, sz) SDL_realloc((p), (sz))
#define DRMP3_FREE(p) SDL_free((p))
#define DRMP3_COPY_MEMORY(dst, src, sz) SDL_memcpy((dst), (src), (sz))
#define DRMP3_MOVE_MEMORY(dst, src, sz) SDL_memmove((dst), (src), (sz))
#define DRMP3_ZERO_MEMORY(p, sz) SDL_memset((p), 0, (sz))

#include "dr_mp3.h"

static size_t mp3_read(void* pUserData, void* pBufferOut, size_t bytesToRead)
{
    Uint8 *ptr = (Uint8 *) pBufferOut;
    Sound_Sample *sample = (Sound_Sample *) pUserData;
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    SDL_IOStream *rwops = internal->rw;
    size_t retval = 0;

    /* !!! FIXME: dr_mp3 treats returning less than bytesToRead as EOF. So we can't EAGAIN. */
    while (bytesToRead)
    {
        const size_t rc = SDL_ReadIO(rwops, ptr, bytesToRead);
        if (rc == 0) break;
        bytesToRead -= rc;
        retval += rc;
        ptr += rc;
    } /* while */

    return retval;
} /* mp3_read */

static drmp3_bool32 mp3_seek(void* pUserData, int offset, drmp3_seek_origin origin)
{
    Sound_Sample *sample = (Sound_Sample *) pUserData;
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    int whence;
    switch (origin) {
    case DRMP3_SEEK_SET:
        whence = SDL_IO_SEEK_SET;
        break;
    case DRMP3_SEEK_CUR:
        whence = SDL_IO_SEEK_CUR;
        break;
    case DRMP3_SEEK_END:
        whence = SDL_IO_SEEK_END;
        break;
    default:
        return DRMP3_FALSE;
    }
    return (SDL_SeekIO(internal->rw, offset, whence) != -1) ? DRMP3_TRUE : DRMP3_FALSE;
} /* mp3_seek */

static drmp3_bool32 mp3_tell(void* pUserData, drmp3_int64* pCursor)
{
    Sound_Sample *sample = (Sound_Sample *) pUserData;
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    *pCursor = SDL_TellIO(internal->rw);
    return (*pCursor != -1) ? DRMP3_TRUE : DRMP3_FALSE;
} /* mp3_tell */


static bool MP3_init(void)
{
    return true; /* always succeeds. */
} /* MP3_init */


static void MP3_quit(void)
{
    /* it's a no-op. */
} /* MP3_quit */

static int MP3_open(Sound_Sample *sample, const char *ext)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    drmp3 *dr = (drmp3 *) SDL_calloc(1, sizeof (drmp3));
    Uint64 frames;

    BAIL_IF_MACRO(!dr, ERR_OUT_OF_MEMORY, 0);
    if (drmp3_init(dr, mp3_read, mp3_seek, mp3_tell, NULL, sample, NULL) != DRMP3_TRUE)
    {
        SDL_free(dr);
        BAIL_IF_MACRO(sample->flags & SOUND_SAMPLEFLAG_ERROR, ERR_IO_ERROR, 0);
        BAIL_MACRO("MP3: Not an MPEG-1 layer 1-3 stream.", 0);
    } /* if */

    SNDDBG(("MP3: Accepting data stream.\n"));
    sample->flags = SOUND_SAMPLEFLAG_CANSEEK;

    sample->actual.channels = dr->channels;
    sample->actual.rate = dr->sampleRate;
    sample->actual.format = SDL_AUDIO_F32; /* dr_mp3 only does float. */

    frames = drmp3_get_pcm_frame_count(dr);
    if (frames == 0) /* ever possible ??? */
        internal->total_time = -1;
    else
    {
        const Uint32 rate = dr->sampleRate;
        internal->total_time = (frames / rate) * 1000;
        internal->total_time += ((frames % rate) * 1000) / rate;
    } /* else */

    internal->decoder_private = dr;

    return 1;
} /* MP3_open */

static void MP3_close(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    drmp3 *dr = (drmp3 *) internal->decoder_private;
    drmp3_uninit(dr);
    SDL_free(dr);
} /* MP3_close */

static Uint32 MP3_read(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    const int channels = (int) sample->actual.channels;
    drmp3 *dr = (drmp3 *) internal->decoder_private;
    const drmp3_uint64 frames_to_read = (internal->buffer_size / channels) / sizeof (float);
    const drmp3_uint64 rc = drmp3_read_pcm_frames_f32(dr, frames_to_read, (float *) internal->buffer);
    /* !!! FIXME: we only set the EOF flags, but this only tells you we're done, not about i/o errors, nor corruption. */
    if (rc < frames_to_read)
        sample->flags |= SOUND_SAMPLEFLAG_EOF;
    return rc * channels * sizeof (float);
} /* MP3_read */

static int MP3_rewind(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    drmp3 *dr = (drmp3 *) internal->decoder_private;
    return (drmp3_seek_to_pcm_frame(dr, 0) == DRMP3_TRUE);
} /* MP3_rewind */

static int MP3_seek(Sound_Sample *sample, Uint32 ms)
{
    Sound_SampleInternal *internal = (Sound_SampleInternal *) sample->opaque;
    drmp3 *dr = (drmp3 *) internal->decoder_private;
    const float frames_per_ms = ((float) sample->actual.rate) / 1000.0f;
    const drmp3_uint64 frame_offset = (drmp3_uint64) (frames_per_ms * ((float) ms));
    return (drmp3_seek_to_pcm_frame(dr, frame_offset) == DRMP3_TRUE);
} /* MP3_seek */

/* dr_mp3 will play layer 1 and 2 files, too */
static const char *extensions_mp3[] = { "MP3", "MP2", "MP1", NULL };
const Sound_DecoderFunctions __Sound_DecoderFunctions_MP3 =
{
    {
        extensions_mp3,
        "MPEG-1 Audio Layer I-III",
        "Ryan C. Gordon <icculus@icculus.org>",
        "https://icculus.org/SDL_sound/"
    },

    MP3_init,       /*   init() method */
    MP3_quit,       /*   quit() method */
    MP3_open,       /*   open() method */
    MP3_close,      /*  close() method */
    MP3_read,       /*   read() method */
    MP3_rewind,     /* rewind() method */
    MP3_seek        /*   seek() method */
};

#endif /* SOUND_SUPPORTS_MP3 */

/* end of SDL_sound_mp3.c ... */

