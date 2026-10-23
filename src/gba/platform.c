/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                         COSMORE GBA PORT PLATFORM                         *
 *                                                                           *
 * Plays the part of the IBM PC hardware the game talks to. Unlike the SDL   *
 * port, the game's interrupt handlers run from real interrupts here: a GBA  *
 * timer stands in for PIT channel 0 (INT 8), and the vblank interrupt reads *
 * the buttons and feeds XT scancodes to the keyboard handler (INT 9).       *
 *                                                                           *
 * Button mapping: D-pad = arrow keys, A = Ctrl (jump), B = Alt (bomb),      *
 * Start = Enter, L = Esc, R = F1 (help menu). Select opens an on-screen     *
 * keyboard for everything else, like the letters in the menus.              *
 *****************************************************************************/

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <gba_interrupt.h>
#include <gba_systemcalls.h>

#include "gbaport.h"

#define REG_IME_        (*(volatile uint16_t *)0x04000208)
#define REG_KEYINPUT_   (*(volatile uint16_t *)0x04000130)
#define REG_WAITCNT_    (*(volatile uint16_t *)0x04000204)
#define REG_DISPCNT_    (*(volatile uint16_t *)0x04000000)
#define BACKDROP_COLOR  (*(volatile uint16_t *)0x05000000)
#define REG_TM2CNT_L_   (*(volatile uint16_t *)0x04000108)
#define REG_TM2CNT_H_   (*(volatile uint16_t *)0x0400010a)
#define TIMER_START     0x0080
#define TIMER_IRQ       0x0040
#define TIMER_DIV_64    0x0001
#define TIMER_HZ        262144UL  /* 16.78 MHz / 64 */
#define PIT_CLOCK_HZ    1193182UL

#define BUTTON_A        0x0001
#define BUTTON_B        0x0002
#define BUTTON_SELECT   0x0004
#define BUTTON_START    0x0008
#define BUTTON_RIGHT    0x0010
#define BUTTON_LEFT     0x0020
#define BUTTON_UP       0x0040
#define BUTTON_DOWN     0x0080
#define BUTTON_R        0x0100
#define BUTTON_L        0x0200
#define BUTTON_DPAD     (BUTTON_UP | BUTTON_DOWN | BUTTON_LEFT | BUTTON_RIGHT)

#define REPEAT_DELAY    18  /* frames */
#define REPEAT_RATE     5

#define FAKE_CORE_BYTES 640000UL

typedef void (*VectorFunction)(void);

volatile uint32_t frameCount;
volatile int isInInterrupt;

static VectorFunction vectors[256];

static uint16_t pitReload[3];
static uint8_t pitAccess[3], pitLowByte[3], pitFlipFlop[3];

static volatile uint8_t port60 = 0x9c;
static uint8_t port61;
static uint8_t oplAddress, oplStatus;

static uint8_t keyQueue[64];
static volatile uint8_t keyQueueHead, keyQueueTail;
static uint16_t lastButtons;

static unsigned long coreUsed;
static uint32_t lastPresentFrame = 0xffffffffUL;

static const struct {
    uint16_t button;
    uint8_t scancode;
} buttonMap[] = {
    {BUTTON_UP,    0x48},  /* up arrow */
    {BUTTON_DOWN,  0x50},  /* down arrow */
    {BUTTON_LEFT,  0x4b},  /* left arrow */
    {BUTTON_RIGHT, 0x4d},  /* right arrow */
    {BUTTON_A,     0x1d},  /* Ctrl */
    {BUTTON_B,     0x38},  /* Alt */
    {BUTTON_START, 0x1c},  /* Enter */
    {BUTTON_L,     0x01},  /* Esc */
    {BUTTON_R,     0x3b}   /* F1 */
};

/*****************************************************************************
 * Keyboard                                                                  *
 *****************************************************************************/

static void QueueKey(uint8_t scancode, int down)
{
    if ((uint8_t)((keyQueueTail + 1) % sizeof keyQueue) == keyQueueHead) return;  /* full */

    keyQueue[keyQueueTail] = (uint8_t)(scancode | (down ? 0x00 : 0x80));
    keyQueueTail = (uint8_t)((keyQueueTail + 1) % sizeof keyQueue);
}

/*
Hand each queued byte to the keyboard interrupt handler, the way the keyboard
controller would raise IRQ 1 once per byte.
*/
static void DeliverKeys(void)
{
    while (keyQueueHead != keyQueueTail) {
        port60 = keyQueue[keyQueueHead];
        keyQueueHead = (uint8_t)((keyQueueHead + 1) % sizeof keyQueue);
        vectors[9]();
    }
}

/*****************************************************************************
 * On-screen keyboard                                                        *
 *****************************************************************************/

typedef struct {
    const char *label;
    uint8_t scancode;
} OverlayKey;

static const OverlayKey overlayRow0[] = {
    {"ESC", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06},
    {"6", 0x07}, {"7", 0x08}, {"8", 0x09}, {"9", 0x0a}, {"0", 0x0b}, {"BKSP", 0x0e},
    {NULL, 0}
};
static const OverlayKey overlayRow1[] = {
    {"TAB", 0x0f}, {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14},
    {"Y", 0x15}, {"U", 0x16}, {"I", 0x17}, {"O", 0x18}, {"P", 0x19},
    {NULL, 0}
};
static const OverlayKey overlayRow2[] = {
    {"A", 0x1e}, {"S", 0x1f}, {"D", 0x20}, {"F", 0x21}, {"G", 0x22}, {"H", 0x23},
    {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"ENTER", 0x1c},
    {NULL, 0}
};
static const OverlayKey overlayRow3[] = {
    {"Z", 0x2c}, {"X", 0x2d}, {"C", 0x2e}, {"V", 0x2f}, {"B", 0x30}, {"N", 0x31},
    {"M", 0x32}, {"SPACE", 0x39},
    {NULL, 0}
};
static const OverlayKey overlayRow4[] = {
    {"F1", 0x3b}, {"F2", 0x3c}, {"F3", 0x3d}, {"F10", 0x44}, {"PGUP", 0x49},
    {"PGDN", 0x51},
    {NULL, 0}
};
static const OverlayKey *const overlayRows[] = {
    overlayRow0, overlayRow1, overlayRow2, overlayRow3, overlayRow4
};
#define OVERLAY_ROWS    5

static volatile int overlayOpen, overlayChanged;
static int cursorRow = 2, cursorCol = 4;
static uint8_t heldOverlayKey;
static uint16_t repeatButton;
static int repeatTimer;

static int OverlayKeyX(int row, int col)
{
    int x = 8, i;

    for (i = 0; i < col; i++) {
        x += (int)(strlen(overlayRows[row][i].label) + 1) * 8;
    }

    return x;
}

static int OverlayRowLength(int row)
{
    int n = 0;

    while (overlayRows[row][n].label != NULL) n++;

    return n;
}

static void OverlayDraw(void)
{
    int row, col;

    VideoFillRect(0, 0, 240, OVERLAY_HEIGHT, OVERLAY_GRAY);

    for (row = 0; row < OVERLAY_ROWS; row++) {
        for (col = 0; overlayRows[row][col].label != NULL; col++) {
            const char *label = overlayRows[row][col].label;
            int x = OverlayKeyX(row, col), y = 3 + (row * 10);
            int selected = row == cursorRow && col == cursorCol;
            int bg = selected ? OVERLAY_YELLOW : OVERLAY_BLUE;

            VideoFillRect(x - 2, y - 1, ((int)strlen(label) * 8) + 4, 9, bg);
            VideoDrawText(x, y, label, selected ? OVERLAY_BLACK : OVERLAY_WHITE, bg);
        }
    }
}

static void MoveCursor(uint16_t button)
{
    if (button & (BUTTON_LEFT | BUTTON_RIGHT)) {
        int n = OverlayRowLength(cursorRow);

        cursorCol = (cursorCol + ((button & BUTTON_LEFT) ? n - 1 : 1)) % n;
    } else {
        /* Find the key in the next row up/down that sits closest */
        int x = OverlayKeyX(cursorRow, cursorCol);
        int row = (cursorRow + ((button & BUTTON_UP) ? OVERLAY_ROWS - 1 : 1)) % OVERLAY_ROWS;
        int best = 0, bestdist = 1000, col;

        for (col = 0; overlayRows[row][col].label != NULL; col++) {
            int dist = abs(OverlayKeyX(row, col) - x);

            if (dist < bestdist) {
                bestdist = dist;
                best = col;
            }
        }
        cursorRow = row;
        cursorCol = best;
    }

    overlayChanged = 1;
}

static void CloseOverlay(void)
{
    if (heldOverlayKey != 0) {
        QueueKey(heldOverlayKey, 0);
        heldOverlayKey = 0;
    }
    overlayOpen = 0;
    VideoShowOverlay(0);
}

static void OverlayButtons(uint16_t pressed, uint16_t released, uint16_t held)
{
    if (pressed & (BUTTON_B | BUTTON_SELECT)) {
        CloseOverlay();
        return;
    }

    if (pressed & BUTTON_A) {
        heldOverlayKey = overlayRows[cursorRow][cursorCol].scancode;
        QueueKey(heldOverlayKey, 1);
    }
    if ((released & BUTTON_A) && heldOverlayKey != 0) {
        QueueKey(heldOverlayKey, 0);
        heldOverlayKey = 0;
    }

    if (pressed & BUTTON_DPAD) {
        repeatButton = pressed & BUTTON_DPAD;
        repeatTimer = REPEAT_DELAY;
        MoveCursor(repeatButton);
    } else if ((held & repeatButton) != 0) {
        if (--repeatTimer == 0) {
            repeatTimer = REPEAT_RATE;
            MoveCursor(repeatButton);
        }
    } else {
        repeatButton = 0;
    }
}

/*****************************************************************************
 * Interrupts                                                                *
 *****************************************************************************/

static void PollButtons(void)
{
    uint16_t held = (uint16_t)(~REG_KEYINPUT_ & 0x03ff);
    uint16_t pressed = held & ~lastButtons;
    uint16_t released = lastButtons & ~held;
    unsigned i;

    lastButtons = held;

    if (overlayOpen) {
        OverlayButtons(pressed, released, held);
        return;
    }

    if (pressed & BUTTON_SELECT) {
        /* Let go of everything first, so nothing stays stuck down */
        for (i = 0; i < sizeof buttonMap / sizeof buttonMap[0]; i++) {
            if (held & buttonMap[i].button) QueueKey(buttonMap[i].scancode, 0);
        }
        lastButtons = BUTTON_SELECT;
        overlayOpen = 1;
        overlayChanged = 1;
        return;
    }

    for (i = 0; i < sizeof buttonMap / sizeof buttonMap[0]; i++) {
        if (pressed & buttonMap[i].button) QueueKey(buttonMap[i].scancode, 1);
        if (released & buttonMap[i].button) QueueKey(buttonMap[i].scancode, 0);
    }
}

static void DefaultVector(void) {}

static void TimerInterrupt(void)
{
    isInInterrupt++;
    vectors[8]();
    isInInterrupt--;
}

static void VBlankInterrupt(void)
{
    AudioVBlank();
    VideoVBlank();
    frameCount++;

    isInInterrupt++;
    PollButtons();
    if (overlayChanged && overlayOpen) {
        overlayChanged = 0;
        OverlayDraw();
        VideoShowOverlay(1);
    }
    DeliverKeys();
    isInInterrupt--;
}

static void UpdatePITRate(void)
{
    uint64_t divisor = pitReload[0] == 0 ? 65536UL : pitReload[0];
    uint64_t ticks = ((divisor * TIMER_HZ) + (PIT_CLOCK_HZ / 2)) / PIT_CLOCK_HZ;

    if (ticks < 1) ticks = 1;
    if (ticks > 0xffff) ticks = 0xffff;

    REG_TM2CNT_H_ = 0;
    REG_TM2CNT_L_ = (uint16_t)(0x10000UL - ticks);
    REG_TM2CNT_H_ = TIMER_START | TIMER_IRQ | TIMER_DIV_64;
}

/*
Fill the screen with one color. Startup shows a different one before each
step, so if a cartridge hangs during startup, the color says where: white means
it never got this far, then red, yellow, blue. VideoInit() turns it black.
*/
static void ShowBootStage(uint16_t bgr)
{
    BACKDROP_COLOR = bgr;
    REG_DISPCNT_ = 0;  /* mode 0, no layers: only the backdrop color shows */
}

/*
Everything is set up before main() runs, so the game can call into the port in
any order.
*/
__attribute__((constructor)) static void PlatformInit(void)
{
    int i;

    ShowBootStage(0x001f);

    /*
    ROM at 4/2 wait states with prefetch, SRAM at 8. Some flash cartridges
    can't keep up with the faster 3/1 and crash right after the switch.
    */
    REG_WAITCNT_ = 0x4003;
    ShowBootStage(0x03ff);

    for (i = 0; i < 256; i++) {
        vectors[i] = DefaultVector;
    }

    FilesInit();
    ShowBootStage(0x7c00);
    VideoInit();
    AudioInit();

    irqInit();
    irqSet(IRQ_VBLANK, VBlankInterrupt);
    irqSet(IRQ_TIMER2, TimerInterrupt);
    irqEnable(IRQ_VBLANK | IRQ_TIMER2);

    UpdatePITRate();
}

/*
Called inside every busy-wait loop. Brings the screen up to date (at most once
per frame), then sleeps until the next interrupt.
*/
void PortIdle(void)
{
    if (isInInterrupt) return;

    if (VideoNeedsPresent() && frameCount != lastPresentFrame) {
        lastPresentFrame = frameCount;
        VideoPresent(0);
        return;
    }

    if (REG_IME_ != 0) Halt();
}

void PortExit(int status)
{
    (void) status;

    REG_IME_ = 0;
    AudioShutdown();

    /* Start over from the cartridge entry point, like turning it off and on */
    *(volatile uint8_t *)0x03007ffa = 0;
    __asm__ volatile ("swi 0x00");

    for (;;);
}

/*****************************************************************************
 * DOS.H                                                                     *
 *****************************************************************************/

void disable(void)
{
    REG_IME_ = 0;
}

void enable(void)
{
    REG_IME_ = 1;
}

void (*getvect(int interruptno))(void)
{
    return vectors[interruptno & 0xff];
}

void setvect(int interruptno, void (*isr)(void))
{
    vectors[interruptno & 0xff] = isr != NULL ? isr : DefaultVector;
}

void outportb(int portid, unsigned char value)
{
    switch (portid) {
    case 0x0040:
    case 0x0041:
    case 0x0042:
        {
            int ch = portid - 0x0040;

            if (pitAccess[ch] == 3) {  /* low byte, then high byte */
                if (pitFlipFlop[ch] == 0) {
                    pitLowByte[ch] = value;
                    pitFlipFlop[ch] = 1;
                    break;
                }
                pitReload[ch] = (uint16_t)(pitLowByte[ch] | (value << 8));
                pitFlipFlop[ch] = 0;
            } else if (pitAccess[ch] == 1) {
                pitReload[ch] = value;
            } else {
                pitReload[ch] = (uint16_t)(value << 8);
            }

            if (ch == 0) UpdatePITRate();
            if (ch == 2) AudioSpeaker((port61 & 0x03) == 0x03, pitReload[2]);
        }
        break;

    case 0x0043:
        {
            int ch = (value >> 6) & 0x03;

            if (ch < 3 && ((value >> 4) & 0x03) != 0) {
                pitAccess[ch] = (value >> 4) & 0x03;
                pitFlipFlop[ch] = 0;
            }
        }
        break;

    case 0x0061:
        port61 = value;
        AudioSpeaker((port61 & 0x03) == 0x03, pitReload[2]);
        break;

    case 0x0388:
        oplAddress = value;
        break;

    case 0x0389:
        /* Just enough of the OPL2 timers to pass the AdLib detection */
        if (oplAddress == 0x04) {
            if (value & 0x80) {
                oplStatus = 0;
            } else if ((value & 0x01) && !(value & 0x40)) {
                oplStatus = 0xc0;
            }
        }
        break;

    case 0x03c4:
    case 0x03c5:
        VideoPortWrite(portid, value);
        break;

    default:  /* PIC, joystick, EGA graphics controller */
        break;
    }
}

void outport(int portid, int value)
{
    outportb(portid, (unsigned char)(value & 0xff));
    outportb(portid + 1, (unsigned char)((value >> 8) & 0xff));
}

unsigned char inportb(int portid)
{
    switch (portid) {
    case 0x0060:
        /* Polling the keyboard is usually part of a wait loop. */
        PortIdle();
        return port60;

    case 0x0061:
        return port61;

    case 0x0201:
        /* No joystick: axis timers expire immediately, buttons not pressed */
        return 0xf0;

    case 0x0388:
        return oplStatus;

    default:
        return 0;
    }
}

int int86(int intno, union REGS *inregs, union REGS *outregs)
{
    if (intno == 0x10 && inregs->h.ah == 0x0f) {
        /* Get Video State */
        outregs->h.al = 0x0d;
        outregs->h.ah = 40;
        outregs->h.bh = 0;
    }

    return outregs->x.ax;
}

/*****************************************************************************
 * CONIO.H                                                                   *
 *****************************************************************************/

void textmode(int newmode)
{
    (void) newmode;
}

int getch(void)
{
    return 0;
}

/* There's no text mode to show the exit screen in. */
void PortShowTextScreen(const unsigned char *buffer)
{
    (void) buffer;
}

/*****************************************************************************
 * ALLOC.H, IO.H, MEM.H, STDLIB.H, STRING.H                                  *
 *****************************************************************************/

/*
A little slack on the end of each block, since the game reads (and sometimes
writes) slightly past some of its buffers.
*/
void *PortMalloc(size_t size)
{
    void *block = calloc(1, size + 16);

    if (block != NULL) coreUsed += ((size + 0x17) >> 4) << 4;

    return block;
}

unsigned long coreleft(void)
{
    return coreUsed < FAKE_CORE_BYTES ? FAKE_CORE_BYTES - coreUsed : 0;
}

long filelength(int handle)
{
    struct stat st;

    if (fstat(handle, &st) != 0) return -1;

    return (long)st.st_size;
}

void movmem(void *src, void *dest, unsigned length)
{
    memmove(dest, src, length);
}

char *ultoa(unsigned long value, char *string, int radix)
{
    char tmp[33];
    int i = 0, j = 0;

    do {
        int digit = (int)(value % radix);
        tmp[i++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
        value /= radix;
    } while (value != 0);

    while (i > 0) {
        string[j++] = tmp[--i];
    }
    string[j] = '\0';

    return string;
}

char *strupr(char *s)
{
    char *p;

    for (p = s; *p != '\0'; p++) {
        *p = (char)toupper((unsigned char)*p);
    }

    return s;
}

static uint32_t randSeed = 1;

int PortRand(void)
{
    randSeed = (randSeed * 0x015a4e35UL) + 1;

    return (int)((randSeed >> 16) & 0x7fff);
}
