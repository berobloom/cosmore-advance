/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                     COSMORE GBA PORT MUSIC PRERENDERER                    *
 *                                                                           *
 * A host tool, run at build time. Finds every music entry in an episode's   *
 * group files, plays it through the SDL port's OPL2 emulator exactly the    *
 * way AdLibService() feeds the chip (560 register-write ticks per second),  *
 * and writes one loop of each song as 8-bit PCM for the GBA:                *
 *                                                                           *
 *     "CMUS", u32 count, u32 sample rate                                    *
 *     count x {char name[16], u32 data offset, u32 length in samples}       *
 *     sample data, each song starting on a 4-byte boundary                  *
 *                                                                           *
 * Usage: musicgen COSMOx.STN COSMOx.VOL output.bin                          *
 *****************************************************************************/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../sdl/opl2.h"

#define OUTPUT_RATE     18157
#define SERVICE_HZ      560
#define MAX_SONGS       64

typedef struct {
    char name[16];
    int16_t *samples;
    size_t count;
} Song;

static Song songs[MAX_SONGS];
static int numSongs;

static uint8_t *ReadFile(const char *path, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;
    long len;

    if (fp == NULL) {
        fprintf(stderr, "musicgen: can't open %s\n", path);
        exit(EXIT_FAILURE);
    }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = malloc((size_t)len);
    if (data == NULL || fread(data, 1, (size_t)len, fp) != (size_t)len) {
        fprintf(stderr, "musicgen: can't read %s\n", path);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
    *size = (size_t)len;

    return data;
}

static uint32_t Le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void OPLWrite(uint8_t addr, uint8_t data)
{
    OPL2WriteAddress(addr);
    OPL2WriteData(data);
}

/*
Play the music data at the chip's native rate, then resample to OUTPUT_RATE
with a box filter.
*/
static void Render(Song *song, const uint8_t *data, size_t length)
{
    size_t left = length & ~(size_t)3, pos = 0, n = 0, cap = 1 << 20;
    uint32_t tick = 0, nextDue = 0;
    double debt = 0.0;
    int16_t *native = malloc(cap * sizeof *native);
    int addr;
    size_t out, i;

    /* State after DetectAdLib() and the StopAdLibPlayback() in SwitchMusic() */
    OPL2Reset();
    for (addr = 0x01; addr <= 0xf5; addr++) OPLWrite((uint8_t)addr, 0);
    OPLWrite(0x01, 0x20);
    OPLWrite(0x08, 0);
    OPLWrite(0xbd, 0);
    for (addr = 0; addr < 10; addr++) OPLWrite((uint8_t)(addr + 0xb1), 0);

    while (left != 0) {
        int count;

        while (left != 0 && nextDue <= tick) {
            nextDue = tick + (data[pos + 2] | (data[pos + 3] << 8));
            OPLWrite(data[pos], data[pos + 1]);
            pos += 4;
            left -= 4;
        }
        tick++;

        debt += (double)OPL2_SAMPLE_RATE / SERVICE_HZ;
        count = (int)debt;
        debt -= count;

        while (count-- > 0) {
            if (n == cap) {
                cap *= 2;
                native = realloc(native, cap * sizeof *native);
            }
            native[n++] = OPL2Generate();
        }
    }

    song->count = (size_t)(((double)n * OUTPUT_RATE) / OPL2_SAMPLE_RATE);
    song->samples = malloc(song->count * sizeof *song->samples);

    for (out = 0; out < song->count; out++) {
        double start = ((double)out * OPL2_SAMPLE_RATE) / OUTPUT_RATE;
        double end = ((double)(out + 1) * OPL2_SAMPLE_RATE) / OUTPUT_RATE;
        double sum = 0.0;

        for (i = (size_t)start; i < (size_t)end + 1 && i < n; i++) {
            double lo = i < start ? start : (double)i;
            double hi = (double)(i + 1) > end ? end : (double)(i + 1);

            if (hi > lo) sum += native[i] * (hi - lo);
        }
        song->samples[out] = (int16_t)(sum / (end - start));
    }

    free(native);
}

static int IsMusicEntry(const char *name)
{
    return name[0] == 'M' && strcmp(name, "MASKTILE.MNI") != 0;
}

static void ScanGroupFile(const uint8_t *data, size_t size)
{
    size_t i;

    for (i = 0; i + 20 <= 980 && i + 20 <= size; i += 20) {
        char name[13];
        uint32_t offset, length;

        if (data[i] == '\0') break;

        memcpy(name, data + i, 12);
        name[12] = '\0';
        offset = Le32(data + i + 12);
        length = Le32(data + i + 16);

        if (!IsMusicEntry(name) || length == 0) continue;
        if ((size_t)offset + length > size || numSongs == MAX_SONGS) continue;

        strncpy(songs[numSongs].name, name, sizeof songs[numSongs].name);
        Render(&songs[numSongs], data + offset, length);
        printf("musicgen: %-12s %6.1f s\n", name, (double)songs[numSongs].count / OUTPUT_RATE);
        numSongs++;
    }
}

static void Put32(FILE *fp, uint32_t value)
{
    uint8_t b[4] = {
        (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)
    };

    fwrite(b, 1, 4, fp);
}

int main(int argc, char *argv[])
{
    size_t size;
    uint8_t *data;
    FILE *fp;
    int peak = 1, s;
    double gain;
    uint32_t offset;

    if (argc != 4) {
        fprintf(stderr, "usage: %s COSMOx.STN COSMOx.VOL output.bin\n", argv[0]);
        return EXIT_FAILURE;
    }

    data = ReadFile(argv[1], &size);
    ScanGroupFile(data, size);
    free(data);

    data = ReadFile(argv[2], &size);
    ScanGroupFile(data, size);
    free(data);

    /* One gain for every song, so they keep their relative loudness */
    for (s = 0; s < numSongs; s++) {
        size_t i;

        for (i = 0; i < songs[s].count; i++) {
            int v = abs(songs[s].samples[i]);

            if (v > peak) peak = v;
        }
    }
    gain = 127.0 / peak;

    fp = fopen(argv[3], "wb");
    if (fp == NULL) {
        fprintf(stderr, "musicgen: can't write %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    fwrite("CMUS", 1, 4, fp);
    Put32(fp, (uint32_t)numSongs);
    Put32(fp, OUTPUT_RATE);

    offset = 12 + (numSongs * 24);
    for (s = 0; s < numSongs; s++) {
        char name[16];

        memset(name, 0, sizeof name);
        strncpy(name, songs[s].name, sizeof name - 1);
        fwrite(name, 1, 16, fp);
        Put32(fp, offset);
        Put32(fp, (uint32_t)songs[s].count);
        offset += (uint32_t)((songs[s].count + 3) & ~(size_t)3);
    }

    for (s = 0; s < numSongs; s++) {
        size_t i;

        for (i = 0; i < songs[s].count; i++) {
            double v = songs[s].samples[i] * gain;
            int8_t b = (int8_t)(v < 0 ? v - 0.5 : v + 0.5);

            fputc((uint8_t)b, fp);
        }
        for (; i % 4 != 0; i++) fputc(0, fp);
    }

    fclose(fp);

    return EXIT_SUCCESS;
}
