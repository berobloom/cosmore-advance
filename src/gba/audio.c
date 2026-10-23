/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE GBA PORT AUDIO                           *
 *                                                                           *
 * Emulating the OPL2 in real time is far beyond the GBA, so every song was  *
 * rendered to 8-bit PCM at build time (see musicgen.c). The game still runs *
 * its AdLib code as usual; it just goes nowhere. The game tells the port    *
 * which song it loaded, and the matching recording streams out of ROM       *
 * through Direct Sound A: timer 0 sets the sample rate and DMA 1 feeds the  *
 * FIFO from a double buffer that is refilled every vblank.                  *
 *                                                                           *
 * The PC speaker is a square wave, which is exactly what the GBA's second   *
 * tone channel makes, so speaker sound effects come out of that channel.    *
 *****************************************************************************/

#include <string.h>
#include <strings.h>

#include "gbaport.h"

#define REG16(addr)         (*(volatile uint16_t *)(addr))
#define REG32(addr)         (*(volatile uint32_t *)(addr))
#define REG_SOUND2CNT_L_    REG16(0x04000068)
#define REG_SOUND2CNT_H_    REG16(0x0400006c)
#define REG_SOUNDCNT_L_     REG16(0x04000080)
#define REG_SOUNDCNT_H_     REG16(0x04000082)
#define REG_SOUNDCNT_X_     REG16(0x04000084)
#define REG_FIFO_A_ADDR     0x040000a0
#define REG_DMA1SAD_        REG32(0x040000bc)
#define REG_DMA1DAD_        REG32(0x040000c0)
#define REG_DMA1CNT_H_      REG16(0x040000c6)
#define REG_TM0CNT_L_       REG16(0x04000100)
#define REG_TM0CNT_H_       REG16(0x04000102)

/* 16.78 MHz / 924 = 18157 Hz, exactly 304 samples per frame */
#define CYCLES_PER_SAMPLE   924
#define SAMPLES_PER_FRAME   304

#define DMA_FIFO_REPEAT     0xb640  /* enable, FIFO timing, 32-bit, repeat, fixed dest */
#define SOUNDCNT_L_BASE     0x0077  /* DMG master volume full, left and right */
#define SOUNDCNT_L_TONE2    0x2200  /* channel 2 to both sides */
#define SOUNDCNT_H_SETUP    0x0b06  /* DMG 100%, DSA 100%, DSA both sides, timer 0, reset */
#define SPEAKER_VOLUME      9  /* of 15 */
#define PIT_CLOCK_HZ        1193182UL

typedef struct {
    char name[16];
    uint32_t offset, length;
} MusicEntry;

typedef struct {
    char magic[4];
    uint32_t count, rate;
    MusicEntry entries[];
} MusicHeader;

extern const uint8_t musicBlob[];

static int8_t mixBuffer[SAMPLES_PER_FRAME * 2] __attribute__((aligned(4)));
static int bufferHalf;

static const MusicEntry *pendingSong;
static const int8_t *volatile song;
static volatile uint32_t songLength, songPos;

static int isSpeakerOn;

void AudioInit(void)
{
    memset(mixBuffer, 0, sizeof mixBuffer);

    REG_SOUNDCNT_X_ = 0x0080;
    REG_SOUNDCNT_L_ = SOUNDCNT_L_BASE;
    REG_SOUNDCNT_H_ = SOUNDCNT_H_SETUP;

    /* Tone channel 2 runs all the time; the speaker gate just mutes it. */
    REG_SOUND2CNT_L_ = (2 << 6) | (SPEAKER_VOLUME << 12);  /* 50% duty */
    REG_SOUND2CNT_H_ = 0x8000 | 1024;

    REG_DMA1SAD_ = (uint32_t)mixBuffer;
    REG_DMA1DAD_ = REG_FIFO_A_ADDR;
    REG_DMA1CNT_H_ = DMA_FIFO_REPEAT;

    REG_TM0CNT_L_ = (uint16_t)(0x10000UL - CYCLES_PER_SAMPLE);
    REG_TM0CNT_H_ = 0x0080;
}

void AudioShutdown(void)
{
    REG_DMA1CNT_H_ = 0;
    REG_TM0CNT_H_ = 0;
    REG_SOUNDCNT_X_ = 0;
}

/* Called from the vblank interrupt */
void AudioVBlank(void)
{
    int8_t *dst = mixBuffer + (bufferHalf * SAMPLES_PER_FRAME);
    int i;

    if (bufferHalf == 1) {
        /* Back to the start, which was filled during the previous frame */
        REG_DMA1CNT_H_ = 0;
        REG_DMA1SAD_ = (uint32_t)mixBuffer;
        REG_DMA1CNT_H_ = DMA_FIFO_REPEAT;
    }

    if (song == NULL || songLength == 0) {
        memset(dst, 0, SAMPLES_PER_FRAME);
    } else {
        uint32_t pos = songPos;

        for (i = 0; i < SAMPLES_PER_FRAME; i++) {
            dst[i] = song[pos];
            if (++pos >= songLength) pos = 0;  /* the music loops */
        }
        songPos = pos;
    }

    bufferHalf ^= 1;
}

void AudioSpeaker(int on, uint16_t divisor)
{
    if (on) {
        unsigned long div = divisor == 0 ? 65536UL : divisor;
        long n = 2048L - (long)((131072UL * div) / PIT_CLOCK_HZ);

        if (n < 0) n = 0;
        if (n > 2047) n = 2047;

        REG_SOUND2CNT_H_ = (uint16_t)n;
        if (!isSpeakerOn) REG_SOUNDCNT_L_ = SOUNDCNT_L_BASE | SOUNDCNT_L_TONE2;
    } else if (isSpeakerOn) {
        REG_SOUNDCNT_L_ = SOUNDCNT_L_BASE;
    }

    isSpeakerOn = on;
}

void PortSetMusic(const char *entry_name)
{
    const MusicHeader *header = (const MusicHeader *)musicBlob;
    uint32_t i;

    pendingSong = NULL;

    for (i = 0; i < header->count; i++) {
        if (strncasecmp(header->entries[i].name, entry_name, sizeof header->entries[i].name) == 0) {
            pendingSong = &header->entries[i];
            break;
        }
    }
}

void PortStartMusic(void)
{
    song = NULL;
    if (pendingSong == NULL) return;

    songPos = 0;
    songLength = pendingSong->length;
    song = (const int8_t *)(musicBlob + pendingSong->offset);
}

void PortStopMusic(void)
{
    song = NULL;
}
