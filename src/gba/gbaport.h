/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                    COSMORE GBA PORT INTERNAL DEFINITIONS                  *
 *                                                                           *
 * Shared between the files in the gba/ directory. The game code never sees  *
 * this header; it talks to the port through the DOS-like functions declared *
 * in sdl/compat.h.                                                          *
 *****************************************************************************/

#ifndef GBA_GBAPORT_H
#define GBA_GBAPORT_H

#include <stdint.h>
#include <stdio.h>

#define SDL_PORT_INTERNAL
#include "../sdl/compat.h"

/* Hot code goes in fast internal RAM; the file holding it is built as ARM. */
#define ARM_IWRAM __attribute__((section(".iwram"), long_call, noinline))
/* Large zero-filled arrays go in external work RAM. */
#define EWRAM_ZERO __attribute__((section(".sbss")))

/* Same as glue.h */
typedef unsigned char byte;
typedef unsigned short word;

/*
Pieces of the game itself that the port needs to reach into.
*/
extern unsigned char *fontTileData;
extern unsigned char isInGame;
FILE *GroupEntryFp(char *entry_name);

/* platform.c */
extern volatile uint32_t frameCount;
extern volatile int isInInterrupt;

/* video.c */
void VideoInit(void);
void VideoPortWrite(int port, uint8_t value);
void VideoPresent(int full);
int VideoNeedsPresent(void);
void VideoRequestFull(void);
void VideoVBlank(void);
void VideoShowOverlay(int show);
void VideoFillRect(int x, int y, int w, int h, int color);
void VideoDrawText(int x, int y, const char *text, int fg, int bg);

/* Overlay canvas: 240 wide, OVERLAY_HEIGHT tall, at the bottom of the screen */
#define OVERLAY_HEIGHT  54
#define OVERLAY_COLORS  6
#define OVERLAY_BLACK   1  /* color 0 is transparent */
#define OVERLAY_GRAY    2
#define OVERLAY_BLUE    3
#define OVERLAY_YELLOW  4
#define OVERLAY_WHITE   5

/* audio.c */
void AudioInit(void);
void AudioVBlank(void);
void AudioSpeaker(int on, uint16_t divisor);
void AudioShutdown(void);

/* files.c */
void FilesInit(void);
const unsigned char *FileROMPointer(FILE *fp);

#endif  /* GBA_GBAPORT_H */
