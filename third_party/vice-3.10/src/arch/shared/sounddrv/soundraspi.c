/*
 * soundraspi.c - Bare metal raspberry pi sound driver
 *
 * Written by
 *  Randy Rossi <randy.rossi@gmail.com>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include "debug.h"
#include "log.h"
#include "sound.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int circle_sound_init(const char *param, int *speed,
                             int *fragsize, int *fragnr, int *channels);
extern int circle_sound_write(int16_t *pbuf, size_t nr);
extern void circle_sound_close(void);
extern int circle_sound_suspend(void);
extern int circle_sound_resume(void);
extern unsigned int circle_sound_bufferspace(void);

#define BMC64_HAYES_AUDIO_OFF 0
#define BMC64_HAYES_AUDIO_DIAL 1
#define BMC64_HAYES_AUDIO_SHORT 2
#define BMC64_HAYES_AUDIO_LONG 3
#define BMC64_MODEM_AUDIO_PHASE_DIAL 0
#define BMC64_MODEM_AUDIO_PHASE_CONNECT 1
#define BMC64_MODEM_AUDIO_PHASES 2
#define BMC64_MODEM_AUDIO_FP_SHIFT 16
#define BMC64_MODEM_AUDIO_BLOCK_FRAMES 512

typedef struct bmc64_modem_audio_sample_s {
    int16_t *data;
    size_t frames;
    unsigned sample_rate;
} bmc64_modem_audio_sample_t;

static int bmc64_modem_audio_mode;
static int bmc64_modem_audio_channels = 1;
static int bmc64_modem_audio_output_rate = 44100;
static bmc64_modem_audio_sample_t bmc64_modem_audio_samples[4][BMC64_MODEM_AUDIO_PHASES];
static bmc64_modem_audio_sample_t *bmc64_modem_audio_active;
static uint64_t bmc64_modem_audio_pos_fp;
static uint64_t bmc64_modem_audio_step_fp = 1ULL << BMC64_MODEM_AUDIO_FP_SHIFT;
#ifdef BMC64_DEBUG_PROFILE
static int bmc64_modem_audio_mix_logged;
#endif

void bmc64_modem_audio_set_mode(int mode);
void bmc64_modem_audio_play_dial_blocking(void);
void bmc64_modem_audio_play_connect_blocking(void);
void bmc64_modem_audio_play_connect_async(void);
void bmc64_modem_audio_stop(void);
static void bmc64_modem_audio_clear_active(void);
static void bmc64_modem_audio_mix(int16_t *pbuf, size_t nr);

static uint16_t bmc64_read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t bmc64_read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char *bmc64_modem_audio_path(int mode, int phase)
{
    if (phase == BMC64_MODEM_AUDIO_PHASE_DIAL &&
        mode >= BMC64_HAYES_AUDIO_DIAL &&
        mode <= BMC64_HAYES_AUDIO_LONG) {
        return "/modem/hayes_dial.wav";
    }
    switch (mode) {
        case BMC64_HAYES_AUDIO_SHORT:
            return phase == BMC64_MODEM_AUDIO_PHASE_CONNECT
                ? "/modem/hayes_short_connect.wav"
                : NULL;
        case BMC64_HAYES_AUDIO_LONG:
            return phase == BMC64_MODEM_AUDIO_PHASE_CONNECT
                ? "/modem/hayes_long_connect.wav"
                : NULL;
        default:
            return NULL;
    }
}

static int bmc64_modem_audio_load_wav(int mode, int phase)
{
    bmc64_modem_audio_sample_t *sample;
    const char *path;
    FILE *fp;
    unsigned char header[44];
    unsigned channels;
    unsigned sample_rate;
    unsigned bits_per_sample;
    uint32_t data_size;
    size_t sample_count;
    int16_t *raw;
    int16_t *mono;
    size_t i;

    if (mode <= BMC64_HAYES_AUDIO_OFF || mode > BMC64_HAYES_AUDIO_LONG ||
        phase < 0 || phase >= BMC64_MODEM_AUDIO_PHASES) {
        return 0;
    }
    sample = &bmc64_modem_audio_samples[mode][phase];
    if (sample->data) {
        return 1;
    }

    path = bmc64_modem_audio_path(mode, phase);
    if (!path) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
#ifdef BMC64_DEBUG_PROFILE
        printf("modemaudiodbg: load failed open mode %d phase %d path %s\r\n",
               mode, phase, path);
#endif
        return 0;
    }

    if (fread(header, 1, sizeof header, fp) != sizeof header ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "WAVEfmt ", 8) != 0 ||
        bmc64_read_le16(header + 20) != 1 ||
        memcmp(header + 36, "data", 4) != 0) {
        fclose(fp);
#ifdef BMC64_DEBUG_PROFILE
        printf("modemaudiodbg: load failed header mode %d phase %d path %s\r\n",
               mode, phase, path);
#endif
        return 0;
    }

    channels = bmc64_read_le16(header + 22);
    sample_rate = bmc64_read_le32(header + 24);
    bits_per_sample = bmc64_read_le16(header + 34);
    data_size = bmc64_read_le32(header + 40);
    if ((channels != 1 && channels != 2) || sample_rate == 0 ||
        bits_per_sample != 16 || data_size < channels * sizeof(int16_t)) {
        fclose(fp);
#ifdef BMC64_DEBUG_PROFILE
        printf("modemaudiodbg: load failed format mode %d phase %d channels %u rate %u bits %u data %lu\r\n",
               mode, phase, channels, sample_rate, bits_per_sample,
               (unsigned long)data_size);
#endif
        return 0;
    }

    sample_count = data_size / sizeof(int16_t);
    raw = (int16_t *)malloc(sample_count * sizeof(int16_t));
    if (!raw) {
        fclose(fp);
#ifdef BMC64_DEBUG_PROFILE
        printf("modemaudiodbg: load failed malloc raw mode %d phase %d samples %lu\r\n",
               mode, phase, (unsigned long)sample_count);
#endif
        return 0;
    }
    if (fread(raw, sizeof(int16_t), sample_count, fp) != sample_count) {
        free(raw);
        fclose(fp);
#ifdef BMC64_DEBUG_PROFILE
        printf("modemaudiodbg: load failed read mode %d phase %d samples %lu\r\n",
               mode, phase, (unsigned long)sample_count);
#endif
        return 0;
    }
    fclose(fp);

    mono = raw;
    if (channels == 2) {
        size_t frames = sample_count / 2;
        mono = (int16_t *)malloc(frames * sizeof(int16_t));
        if (!mono) {
            free(raw);
#ifdef BMC64_DEBUG_PROFILE
            printf("modemaudiodbg: load failed malloc mono mode %d phase %d frames %lu\r\n",
                   mode, phase, (unsigned long)frames);
#endif
            return 0;
        }
        for (i = 0; i < frames; ++i) {
            mono[i] = (int16_t)(((int)raw[i * 2] + (int)raw[i * 2 + 1]) / 2);
        }
        free(raw);
        sample_count = frames;
    }

    sample->data = mono;
    sample->frames = sample_count;
    sample->sample_rate = sample_rate;
#ifdef BMC64_DEBUG_PROFILE
    printf("modemaudiodbg: loaded mode %d phase %d path %s frames %lu rate %u source_channels %u\r\n",
           mode, phase, path, (unsigned long)sample->frames,
           sample->sample_rate, channels);
#endif
    return 1;
}

void bmc64_modem_audio_set_mode(int mode)
{
    int previous_mode = bmc64_modem_audio_mode;

    if (mode < BMC64_HAYES_AUDIO_OFF || mode > BMC64_HAYES_AUDIO_LONG) {
        mode = BMC64_HAYES_AUDIO_OFF;
    }
    if (previous_mode != mode || mode == BMC64_HAYES_AUDIO_OFF) {
        bmc64_modem_audio_stop();
    }
    bmc64_modem_audio_mode = mode;
#ifdef BMC64_DEBUG_PROFILE
    printf("modemaudiodbg: set mode %d\r\n", mode);
#endif
    if (mode == BMC64_HAYES_AUDIO_OFF) {
        return;
    }
    bmc64_modem_audio_load_wav(mode, BMC64_MODEM_AUDIO_PHASE_DIAL);
    if (mode == BMC64_HAYES_AUDIO_SHORT || mode == BMC64_HAYES_AUDIO_LONG) {
        bmc64_modem_audio_load_wav(mode, BMC64_MODEM_AUDIO_PHASE_CONNECT);
    }
}

static int bmc64_modem_audio_start_phase(int phase)
{
    bmc64_modem_audio_sample_t *sample;

    if (bmc64_modem_audio_mode <= BMC64_HAYES_AUDIO_OFF ||
        bmc64_modem_audio_mode > BMC64_HAYES_AUDIO_LONG) {
        return 0;
    }
    bmc64_modem_audio_stop();
    if (!bmc64_modem_audio_load_wav(bmc64_modem_audio_mode, phase)) {
        return 0;
    }

    sample = &bmc64_modem_audio_samples[bmc64_modem_audio_mode][phase];
    if (!sample->data || !sample->frames) {
        return 0;
    }

    bmc64_modem_audio_active = sample;
    bmc64_modem_audio_pos_fp = 0;
#ifdef BMC64_DEBUG_PROFILE
    bmc64_modem_audio_mix_logged = 0;
#endif
    bmc64_modem_audio_step_fp =
        ((uint64_t)sample->sample_rate << BMC64_MODEM_AUDIO_FP_SHIFT) /
        (uint64_t)bmc64_modem_audio_output_rate;
    if (bmc64_modem_audio_step_fp == 0) {
        bmc64_modem_audio_step_fp = 1ULL << BMC64_MODEM_AUDIO_FP_SHIFT;
    }
#ifdef BMC64_DEBUG_PROFILE
    printf("modemaudiodbg: play mode %d phase %d frames %lu source_rate %u output_rate %d channels %d step %lu\r\n",
           bmc64_modem_audio_mode, phase, (unsigned long)sample->frames,
           sample->sample_rate, bmc64_modem_audio_output_rate,
           bmc64_modem_audio_channels,
           (unsigned long)bmc64_modem_audio_step_fp);
#endif
    return 1;
}

static void bmc64_modem_audio_play_phase_blocking(int phase)
{
    int channels;
    size_t samples;
    int16_t buffer[BMC64_MODEM_AUDIO_BLOCK_FRAMES * 2];

    if (!bmc64_modem_audio_start_phase(phase)) {
        return;
    }

    channels = bmc64_modem_audio_channels > 0 ? bmc64_modem_audio_channels : 1;
    if (channels > 2) {
        channels = 2;
    }
    samples = BMC64_MODEM_AUDIO_BLOCK_FRAMES * (size_t)channels;

    while (bmc64_modem_audio_active) {
        memset(buffer, 0, samples * sizeof(buffer[0]));
        bmc64_modem_audio_mix(buffer, samples);
        circle_sound_write(buffer, samples);
    }
}

void bmc64_modem_audio_play_dial_blocking(void)
{
    bmc64_modem_audio_play_phase_blocking(BMC64_MODEM_AUDIO_PHASE_DIAL);
}

void bmc64_modem_audio_play_connect_blocking(void)
{
    bmc64_modem_audio_play_phase_blocking(BMC64_MODEM_AUDIO_PHASE_CONNECT);
}

void bmc64_modem_audio_play_connect_async(void)
{
    bmc64_modem_audio_start_phase(BMC64_MODEM_AUDIO_PHASE_CONNECT);
}

void bmc64_modem_audio_stop(void)
{
    if (!bmc64_modem_audio_active && bmc64_modem_audio_pos_fp == 0) {
        return;
    }
    bmc64_modem_audio_clear_active();
#ifdef BMC64_DEBUG_PROFILE
    printf("modemaudiodbg: stop\r\n");
#endif
}

static void bmc64_modem_audio_clear_active(void)
{
    bmc64_modem_audio_active = NULL;
    bmc64_modem_audio_pos_fp = 0;
#ifdef BMC64_DEBUG_PROFILE
    bmc64_modem_audio_mix_logged = 0;
#endif
}

static void bmc64_modem_audio_mix(int16_t *pbuf, size_t nr)
{
    bmc64_modem_audio_sample_t *active;
    size_t frame;
    size_t frames;
    int channels;

    active = bmc64_modem_audio_active;
    if (!active || !active->data ||
        bmc64_modem_audio_channels <= 0) {
        return;
    }

    channels = bmc64_modem_audio_channels;
    frames = nr / (size_t)channels;
#ifdef BMC64_DEBUG_PROFILE
    if (!bmc64_modem_audio_mix_logged) {
        printf("modemaudiodbg: mix start nr %lu frames %lu channels %d\r\n",
               (unsigned long)nr, (unsigned long)frames, channels);
        bmc64_modem_audio_mix_logged = 1;
    }
#endif
    for (frame = 0; frame < frames; ++frame) {
        active = bmc64_modem_audio_active;
        if (!active || !active->data) {
            return;
        }
        size_t pos = (size_t)(bmc64_modem_audio_pos_fp >>
                              BMC64_MODEM_AUDIO_FP_SHIFT);
        int sample;
        int channel;

        if (pos >= active->frames) {
            bmc64_modem_audio_clear_active();
            return;
        }

        sample = active->data[pos] / 4;
        for (channel = 0; channel < channels; ++channel) {
            size_t index = frame * (size_t)channels + (size_t)channel;
            int mixed = (int)pbuf[index] + sample;
            if (mixed > 32767) {
                mixed = 32767;
            } else if (mixed < -32768) {
                mixed = -32768;
            }
            pbuf[index] = (int16_t)mixed;
        }

        bmc64_modem_audio_pos_fp += bmc64_modem_audio_step_fp;
    }
}

static int raspi_init(const char *param, int *speed, int *fragsize, int *fragnr, int *channels)
{
    int rc = circle_sound_init(param, speed, fragsize, fragnr, channels);
    bmc64_modem_audio_output_rate = *speed > 0 ? *speed : 44100;
    bmc64_modem_audio_channels = *channels > 0 ? *channels : 1;
    return rc;
}

static int raspi_write(int16_t *pbuf, size_t nr)
{
    bmc64_modem_audio_mix(pbuf, nr);
    return circle_sound_write(pbuf, nr);
}

static void raspi_close(void)
{
   circle_sound_close();
}

static int raspi_suspend(void)
{
   return circle_sound_suspend();
}

static int raspi_resume(void)
{
   return circle_sound_resume();
}

static int raspi_bufferspace(void) {
  return circle_sound_bufferspace();
}

static sound_device_t raspi_device =
{
    "raspi",
    raspi_init,
    raspi_write,
    NULL,
    NULL,
    raspi_bufferspace,
    raspi_close,
    raspi_suspend,
    raspi_resume,
    1,
    2,
    true
};

int sound_init_raspi_device(void)
{
    return sound_register_device(&raspi_device);
}
