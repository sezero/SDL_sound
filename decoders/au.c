/*
 * SDL_sound -- An abstract sound format decoding API.
 * Copyright (C) 2001  Ryan C. Gordon.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Sun/NeXT .au decoder for SDL_sound.
 * Formats supported: 8 and 16 bit linear PCM, 8 bit �-law.
 * Files without valid header are assumed to be 8 bit �-law, 8kHz, mono.
 *
 * Please see the file COPYING in the source's root directory.
 *
 *  This file written by Mattias Engdeg�rd. (f91-men@nada.kth.se)
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif

#ifdef SOUND_SUPPORTS_AU

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "SDL_sound.h"

#define __SDL_SOUND_INTERNAL__
#include "SDL_sound_internal.h"

static int AU_init(void);
static void AU_quit(void);
static int AU_open(Sound_Sample *sample, const char *ext);
static void AU_close(Sound_Sample *sample);
static Uint32 AU_read(Sound_Sample *sample);

/*
 * Sometimes the extension ".snd" is used for these files (mostly on the NeXT),
 * and the magic number comes from this. However it may clash with other
 * formats and is somewhat of an anachronism, so only .au is used here.
 */
static const char *extensions_au[] = { "AU", NULL };
const Sound_DecoderFunctions __Sound_DecoderFunctions_AU =
{
    {
        extensions_au,
        "Sun/NeXT audio file format",
        "Mattias Engdeg�rd <f91-men@nada.kth.se>",
        "http://www.icculus.org/SDL_sound/"
    },

    AU_init,
    AU_quit,
    AU_open,
    AU_close,
    AU_read
};

/* no init/deinit needed */
static int AU_init(void)
{
    return(1);
}

static void AU_quit(void)
{
    /* no-op. */
}

struct au_file_hdr
{
    Uint32 magic;
    Uint32 hdr_size;
    Uint32 data_size;
    Uint32 encoding;
    Uint32 sample_rate;
    Uint32 channels;
};

#define HDR_SIZE 24

enum
{
    AU_ENC_ULAW_8       = 1,        /* 8-bit ISDN �-law */
    AU_ENC_LINEAR_8     = 2,        /* 8-bit linear PCM */
    AU_ENC_LINEAR_16    = 3,        /* 16-bit linear PCM */

    /* the rest are unsupported (I have never seen them in the wild) */
    AU_ENC_LINEAR_24    = 4,        /* 24-bit linear PCM */
    AU_ENC_LINEAR_32    = 5,        /* 32-bit linear PCM  */
    AU_ENC_FLOAT        = 6,        /* 32-bit IEEE floating point */
    AU_ENC_DOUBLE       = 7,        /* 64-bit IEEE floating point */
    /* more Sun formats, not supported either */
    AU_ENC_ADPCM_G721   = 23,
    AU_ENC_ADPCM_G722   = 24,
    AU_ENC_ADPCM_G723_3 = 25,
    AU_ENC_ADPCM_G723_5 = 26,
    AU_ENC_ALAW_8       = 27
};

struct audec
{
    unsigned remaining;
    int encoding;
};


#define AU_MAGIC 0x646E732E  /* ".snd", in ASCII */

static int AU_open(Sound_Sample *sample, const char *ext)
{
    Sound_SampleInternal *internal = sample->opaque;
    SDL_RWops *rw = internal->rw;
    int r, skip, hsize, i;
    struct au_file_hdr hdr;
    char c;
    struct audec *dec = malloc(sizeof *dec);
    BAIL_IF_MACRO(dec == NULL, ERR_OUT_OF_MEMORY, 0);
    internal->decoder_private = dec;

    r = SDL_RWread(rw, &hdr, 1, HDR_SIZE);
    if (r < HDR_SIZE)
    {
        Sound_SetError("No .au file (bad header)");
        free(dec);
        return 0;
    }

    if (SDL_SwapLE32(hdr.magic) == AU_MAGIC)
    {
        /* valid magic */
        dec->encoding = SDL_SwapBE32(hdr.encoding);
        switch(dec->encoding)
        {
            case AU_ENC_ULAW_8:
                /* Convert 8-bit �-law to 16-bit linear on the fly. This is
                   slightly wasteful if the audio driver must convert them
                   back, but �-law only devices are rare (mostly _old_ Suns) */
                sample->actual.format = AUDIO_S16SYS;
                break;

            case AU_ENC_LINEAR_8:
                sample->actual.format = AUDIO_S8;
                break;

            case AU_ENC_LINEAR_16:
                sample->actual.format = AUDIO_S16MSB;
                break;

            default:
                Sound_SetError("Unsupported .au encoding");
                free(dec);
                return 0;
        }

        sample->actual.rate = SDL_SwapBE32(hdr.sample_rate);
        sample->actual.channels = SDL_SwapBE32(hdr.channels);
        dec->remaining = SDL_SwapBE32(hdr.data_size);
        hsize = SDL_SwapBE32(hdr.hdr_size);

        /* skip remaining part of header (input may be unseekable) */
        for (i = HDR_SIZE; i < hsize; i++)
            SDL_RWread(rw, &c, 1, 1);
    }

    else if (__Sound_strcasecmp(ext, "au") == 0)
    {
        /*
         * A number of files in the wild have the .au extension but no valid
         * header; these are traditionally assumed to be 8kHz �-law. Handle
         * them here only if the extension is recognized.
         */

        SNDDBG(("AU: Invalid header, assuming raw 8kHz �-law.\n"));
        /* if seeking fails, we lose 24 samples. big deal */
        SDL_RWseek(rw, -HDR_SIZE, SEEK_CUR);
        dec->encoding = AU_ENC_ULAW_8;
        dec->remaining = (Uint32)-1; 		/* no limit */
        sample->actual.format = AUDIO_S16SYS;
        sample->actual.rate = 8000;
        sample->actual.channels = 1;
    }

    sample->flags = SOUND_SAMPLEFLAG_NONE;

    SNDDBG(("AU: Accepting data stream.\n"));
    return 1;
}


static void AU_close(Sound_Sample *sample)
{
    Sound_SampleInternal *internal = sample->opaque;
    free(internal->decoder_private);
}

/* table to convert from �-law encoding to signed 16-bit samples,
   generated by a throwaway perl script */
static Sint16 ulaw_to_linear[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0
};

static Uint32 AU_read(Sound_Sample *sample)
{
    int ret;
    Sound_SampleInternal *internal = sample->opaque;
    struct audec *dec = internal->decoder_private;
    int maxlen;
    Uint8 *buf;

    maxlen = internal->buffer_size;
    buf = internal->buffer;
    if (dec->encoding == AU_ENC_ULAW_8)
    {
        /* We read �-law samples into the second half of the buffer, so
           we can expand them to 16-bit samples afterwards */
        maxlen >>= 1;
        buf += maxlen;
    }

    if(maxlen > dec->remaining)
        maxlen = dec->remaining;
    ret = SDL_RWread(internal->rw, buf, 1, maxlen);
    if (ret == 0)
        sample->flags |= SOUND_SAMPLEFLAG_EOF;
    else if (ret == -1)
        sample->flags |= SOUND_SAMPLEFLAG_ERROR;
    else
    {
        dec->remaining -= ret;
        if (ret < maxlen)
            sample->flags |= SOUND_SAMPLEFLAG_EAGAIN;

        if (dec->encoding == AU_ENC_ULAW_8)
        {
            int i;
            Sint16 *dst = internal->buffer;
            for (i = 0; i < ret; i++)
                dst[i] = ulaw_to_linear[buf[i]];
            ret <<= 1;                  /* return twice as much as read */
        }
    }

    return ret;
}

#endif /* SOUND_SUPPORTS_AU */
