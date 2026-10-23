/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                    COSMORE PORT COMPATIBILITY HEADER                      *
 *                                                                           *
 * Stands in for the Borland/DOS headers (ALLOC.H, CONIO.H, DOS.H, IO.H, and *
 * MEM.H) when the game is compiled with a modern compiler. Everything that  *
 * touched hardware -- I/O ports, interrupt vectors, the EGA, the PIT -- is  *
 * routed to the emulation layer in the sdl/ directory, or in the gba/       *
 * directory when PORT_GBA is defined.                                       *
 *****************************************************************************/

#ifndef SDL_COMPAT_H
#define SDL_COMPAT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Borland keywords that have no meaning here. */
#define interrupt
#define far
#define huge

/* From CONIO.H */
#define C80 3

enum COLORS {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHTGRAY, DARKGRAY,
    LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, WHITE
};

/* From DOS.H */
union REGS {
    struct {unsigned short ax, bx, cx, dx, si, di, cflag, flags;} x;
    struct {unsigned char al, ah, bl, bh, cl, ch, dl, dh;} h;
};

unsigned long coreleft(void);
void disable(void);
void enable(void);
long filelength(int handle);
int getch(void);
void (*getvect(int interruptno))(void);
void setvect(int interruptno, void (*isr)(void));
unsigned char inportb(int portid);
void outportb(int portid, unsigned char value);
void outport(int portid, int value);
int int86(int intno, union REGS *inregs, union REGS *outregs);
void movmem(void *src, void *dest, unsigned length);
void textmode(int newmode);
char *ultoa(unsigned long value, char *string, int radix);
char *strupr(char *s);

/*
Turbo C's rand() is a specific LCG with RAND_MAX of 0x7fff. Some game logic
(lightning, for instance) depends on that range, so it is replicated exactly.
*/
int PortRand(void);

/*
Memory and file shims. DOS was far more forgiving about small heap overruns,
fclose(NULL), and case-insensitive filenames; getw()/putw() were 16-bit.
*/
void *PortMalloc(size_t size);
FILE *PortFopen(const char *filename, const char *mode);
int PortFclose(FILE *fp);
int PortRemove(const char *filename);
int PortGetw(FILE *fp);
int PortPutw(int w, FILE *fp);

/*
Everything below replaces standard library functions by name. The emulation
layer itself defines SDL_PORT_INTERNAL to see the real ones.
*/
#ifndef SDL_PORT_INTERNAL
#undef RAND_MAX
#define RAND_MAX 0x7fff
#define rand() PortRand()
#define random(num) ((int)(((long)PortRand() * (num)) / (RAND_MAX + 1)))
#define malloc(size) PortMalloc(size)
#define fopen(name, mode) PortFopen((name), (mode))
#define fclose(fp) PortFclose(fp)
#define remove(name) PortRemove(name)
#define getw(fp) PortGetw(fp)
#define putw(w, fp) PortPutw((w), (fp))
#endif  /* SDL_PORT_INTERNAL */

/*
Replacements for the few places where the game wrote directly into EGA or text
mode memory with a far pointer.
*/
void EGAWriteByte(unsigned short offset, unsigned char value);
void PortShowTextScreen(const unsigned char *buffer);

/*
Called inside busy-wait loops, where the original game spun until an interrupt
handler modified a global variable. Services timers, input, audio, and video.
*/
void PortIdle(void);
#define SYSTEM_IDLE() PortIdle()

#ifdef PORT_GBA
/*
Game Boy Advance hooks. Read-only data is used in place in cartridge ROM, so
reading a group entry into its own ROM address is a no-op. There is no EGA
memory, so tile copies only record where the tile images live. Music is played
from prerecorded audio instead of being sent to an OPL2 chip.
*/
unsigned char *PortGroupEntryROM(char *entry_name);
size_t PortFread(void *ptr, size_t size, size_t count, FILE *fp);
void PortCopyTilesToEGA(unsigned char *source, unsigned short length, unsigned short offset);
void PortBeginMapRegion(void);
void PortSetMusic(const char *entry_name);
void PortStartMusic(void);
void PortStopMusic(void);
void PortExit(int status);

/* D-pad menus (gba/menu.c); each returns the scancode the DOS prompt would read */
void PortDrawMainMenu(void);
unsigned char PortMainMenuSelect(void);
void PortDrawHelpMenu(void);
unsigned char PortHelpMenuSelect(void);
unsigned char PortSelectGameNumber(unsigned short x, unsigned short y);

#ifndef SDL_PORT_INTERNAL
#define fread(ptr, size, count, fp) PortFread((ptr), (size), (count), (fp))
#define exit(status) PortExit(status)
#endif  /* SDL_PORT_INTERNAL */
#endif  /* PORT_GBA */

#endif  /* SDL_COMPAT_H */
