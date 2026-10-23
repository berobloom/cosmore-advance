/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE SDL PORT PLATFORM                        *
 *                                                                           *
 * Emulates the slice of the IBM PC that the game talks to: I/O ports for    *
 * the PIT, PIC, keyboard controller, PC speaker, AdLib, and EGA, plus the   *
 * interrupt vector table for the timer (INT 8) and keyboard (INT 9).        *
 *                                                                           *
 * Everything runs on the main thread. Whenever the game waits -- spinning   *
 * on a variable that an interrupt handler would have changed -- it calls    *
 * PortIdle(), which catches the emulated timer up to wall-clock time by     *
 * invoking the game's own interrupt handlers, delivers pending keystrokes,  *
 * produces the audio for the elapsed time, and presents the screen.         *
 *****************************************************************************/

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>

#define SDL_PORT_INTERNAL
#include "compat.h"
#include "ega.h"
#include "opl2.h"
#include "testhooks.h"

#define PIT_CLOCK_HZ    1193182.0
#define AUDIO_RATE      OPL2_SAMPLE_RATE
#define SPEAKER_VOLUME  5000
#define MAX_AUDIO_QUEUE (AUDIO_RATE / 6)  /* in samples; about 167 ms */
#define PRESENT_HZ      70.0
#define MAX_CATCHUP_S   0.25

#define HEAP_PADDING    0x10000L
#define FAKE_CORE_BYTES 640000UL

typedef void (*VectorFunction)(void);

static int isInitialized = 0;
static int isInInterrupt = 0;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *screenTexture, *scaledTexture;
static uint32_t framePixels[320 * 200];
static int scaleFactor = 4;
static SDL_AudioDeviceID audioDevice;

static VectorFunction vectors[256];

/* PIT state */
static uint16_t pitReload[3];
static uint8_t pitAccess[3], pitLowByte[3], pitFlipFlop[3];
static double pitRate;
static double nextTickTime, nextPresentTime;

/* Keyboard controller and PC speaker state */
static uint8_t port60 = 0x9c;
static uint8_t port61;
static uint8_t keyQueue[256];
static uint8_t keyQueueHead, keyQueueTail;
static uint8_t keyIsDown[SDL_NUM_SCANCODES];

/* Audio synthesis state */
static int16_t audioBuffer[8192];
static int audioBufferLen;
static double audioSampleDebt;
static double speakerPhase;

static unsigned long coreUsed;

static double Now(void)
{
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

static void DefaultVector(void) {}

static void UpdatePITRate(void)
{
    double divisor = pitReload[0] == 0 ? 65536.0 : (double)pitReload[0];

    pitRate = PIT_CLOCK_HZ / divisor;
    nextTickTime = Now() + (1.0 / pitRate);
}

static void Shutdown(void)
{
    if (audioDevice != 0) SDL_CloseAudioDevice(audioDevice);
    SDL_Quit();
}

static void PlatformInit(void)
{
    SDL_AudioSpec want, have;
    int i;

    if (isInitialized) return;
    isInitialized = 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    atexit(Shutdown);

    window = SDL_CreateWindow(
        "Cosmo's Cosmic Adventure", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 720, SDL_WINDOW_RESIZABLE
    );
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer == NULL) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    screenTexture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 200
    );
    SDL_SetTextureScaleMode(screenTexture, SDL_ScaleModeNearest);

    /*
    Scale up by an integer factor with sharp pixels first, then let the final
    (non-integer, aspect-corrected) stretch use linear filtering. This keeps the
    pixels crisp without uneven row heights.
    */
    scaledTexture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET,
        320 * scaleFactor, 200 * scaleFactor
    );
    if (scaledTexture != NULL) {
        SDL_SetTextureScaleMode(scaledTexture, SDL_ScaleModeLinear);
    }

    SDL_zero(want);
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audioDevice != 0) {
        int16_t silence[1024];

        memset(silence, 0, sizeof silence);
        SDL_QueueAudio(audioDevice, silence, sizeof silence);
        SDL_PauseAudioDevice(audioDevice, 0);
    } else {
        fprintf(stderr, "No audio: %s\n", SDL_GetError());
    }

    OPL2Reset();

    for (i = 0; i < 256; i++) {
        vectors[i] = DefaultVector;
    }

    UpdatePITRate();
    nextPresentTime = Now();
}

/*****************************************************************************
 * Video                                                                     *
 *****************************************************************************/

static void Present(void)
{
    void *pixels;
    int pitch, winw, winh;
    SDL_Rect dest;

    EGARenderFrame(framePixels);

    if (SDL_LockTexture(screenTexture, NULL, &pixels, &pitch) == 0) {
        int y;

        for (y = 0; y < 200; y++) {
            memcpy((uint8_t *)pixels + (y * pitch), framePixels + (y * 320), 320 * 4);
        }
        SDL_UnlockTexture(screenTexture);
    }

    /* Fit a 4:3 picture into the window */
    SDL_GetRendererOutputSize(renderer, &winw, &winh);
    if (winw * 3 > winh * 4) {
        dest.h = winh;
        dest.w = (winh * 4) / 3;
    } else {
        dest.w = winw;
        dest.h = (winw * 3) / 4;
    }
    dest.x = (winw - dest.w) / 2;
    dest.y = (winh - dest.h) / 2;

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    if (scaledTexture != NULL) {
        SDL_SetRenderTarget(renderer, scaledTexture);
        SDL_RenderCopy(renderer, screenTexture, NULL, NULL);
        SDL_SetRenderTarget(renderer, NULL);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, scaledTexture, NULL, &dest);
    } else {
        SDL_RenderCopy(renderer, screenTexture, NULL, &dest);
    }

    SDL_RenderPresent(renderer);
}

/*****************************************************************************
 * Audio                                                                     *
 *****************************************************************************/

static void FlushAudio(void)
{
    if (audioBufferLen == 0) return;

#ifdef PORT_TEST_HOOKS
    TestHookAudio(audioBuffer, audioBufferLen);
#endif  /* PORT_TEST_HOOKS */

    if (audioDevice != 0) {
        Uint32 queued = SDL_GetQueuedAudioSize(audioDevice) / sizeof(int16_t);

        /* If the queue has backed up (e.g. the window was being dragged), drop
        this chunk rather than letting the latency grow without bound. */
        if (queued < MAX_AUDIO_QUEUE) {
            SDL_QueueAudio(audioDevice, audioBuffer, audioBufferLen * sizeof(int16_t));
        }
    }

    audioBufferLen = 0;
}

/*
Produce audio for one PIT tick's worth of time: the AdLib output plus the PC
speaker square wave (when both gate and data bits of port 61h are on).
*/
static void GenerateAudio(void)
{
    int count, i;
    double speakerStep = 0.0;
    int speakerOn = (port61 & 0x03) == 0x03;

    audioSampleDebt += (double)AUDIO_RATE / pitRate;
    count = (int)audioSampleDebt;
    audioSampleDebt -= count;

    if (speakerOn) {
        double divisor = pitReload[2] == 0 ? 65536.0 : (double)pitReload[2];
        speakerStep = (PIT_CLOCK_HZ / divisor) / AUDIO_RATE;
    }

    for (i = 0; i < count; i++) {
        int32_t sample = OPL2Generate();

        if (speakerOn) {
            sample += speakerPhase < 0.5 ? SPEAKER_VOLUME : -SPEAKER_VOLUME;
            speakerPhase += speakerStep;
            speakerPhase -= (int)speakerPhase;
        }

        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        if (audioBufferLen == (int)(sizeof audioBuffer / sizeof audioBuffer[0])) {
            FlushAudio();
        }
        audioBuffer[audioBufferLen++] = (int16_t)sample;
    }
}

/*****************************************************************************
 * Keyboard                                                                  *
 *****************************************************************************/

/*
Translate an SDL scancode into an XT (set 1) make code. Bit 8 set in the result
means the key sends an E0h prefix byte. Zero means "no equivalent."
*/
static int XTScancode(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02;
    case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06;
    case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08;
    case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0a;
    case SDL_SCANCODE_0: return 0x0b;
    case SDL_SCANCODE_MINUS: return 0x0c;
    case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e;
    case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_Q: return 0x10;
    case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14;
    case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16;
    case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_A: return 0x1e;
    case SDL_SCANCODE_S: return 0x1f;
    case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22;
    case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24;
    case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a;
    case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_Z: return 0x2c;
    case SDL_SCANCODE_X: return 0x2d;
    case SDL_SCANCODE_C: return 0x2e;
    case SDL_SCANCODE_V: return 0x2f;
    case SDL_SCANCODE_B: return 0x30;
    case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_F1: return 0x3b;
    case SDL_SCANCODE_F2: return 0x3c;
    case SDL_SCANCODE_F3: return 0x3d;
    case SDL_SCANCODE_F4: return 0x3e;
    case SDL_SCANCODE_F5: return 0x3f;
    case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41;
    case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_KP_4: return 0x4b;
    case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_KP_6: return 0x4d;
    case SDL_SCANCODE_KP_PLUS: return 0x4e;
    case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;

    /* Keys added by the 101-key keyboard, which send an E0h prefix */
    case SDL_SCANCODE_KP_ENTER: return 0x11c;
    case SDL_SCANCODE_RCTRL: return 0x11d;
    case SDL_SCANCODE_KP_DIVIDE: return 0x135;
    case SDL_SCANCODE_RALT: return 0x138;
    case SDL_SCANCODE_HOME: return 0x147;
    case SDL_SCANCODE_UP: return 0x148;
    case SDL_SCANCODE_PAGEUP: return 0x149;
    case SDL_SCANCODE_LEFT: return 0x14b;
    case SDL_SCANCODE_RIGHT: return 0x14d;
    case SDL_SCANCODE_END: return 0x14f;
    case SDL_SCANCODE_DOWN: return 0x150;
    case SDL_SCANCODE_PAGEDOWN: return 0x151;
    case SDL_SCANCODE_INSERT: return 0x152;
    case SDL_SCANCODE_DELETE: return 0x153;

    default: return 0;
    }
}

static void QueueScancodeByte(uint8_t value)
{
    if ((uint8_t)(keyQueueTail + 1) == keyQueueHead) return;  /* full */

    keyQueue[keyQueueTail++] = value;
}

static void QueueKey(SDL_Scancode sc, int down)
{
    int xt = XTScancode(sc);

    if (xt == 0) return;

    keyIsDown[sc] = (uint8_t)down;

    if (xt & 0x100) QueueScancodeByte(0xe0);
    QueueScancodeByte((uint8_t)((xt & 0x7f) | (down ? 0x00 : 0x80)));
}

static void ReleaseAllKeys(void)
{
    int sc;

    for (sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
        if (keyIsDown[sc]) QueueKey((SDL_Scancode)sc, 0);
    }
}

/*
Hand one byte to the keyboard interrupt handler, the way the keyboard
controller would raise IRQ 1 once per byte.
*/
static void DeliverKeyboardByte(void)
{
    if (keyQueueHead == keyQueueTail) return;

    port60 = keyQueue[keyQueueHead++];

    isInInterrupt = 1;
    vectors[9]();
    isInInterrupt = 0;
}

static void ToggleFullscreen(void)
{
    Uint32 flags = SDL_GetWindowFlags(window);

    SDL_SetWindowFullscreen(
        window, (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP
    );
}

#ifdef PORT_TEST_HOOKS
static void SaveScreenshot(const char *path)
{
    SDL_Surface *surface;

    EGARenderFrame(framePixels);
    surface = SDL_CreateRGBSurfaceWithFormatFrom(
        framePixels, 320, 200, 32, 320 * 4, SDL_PIXELFORMAT_ARGB8888
    );
    if (surface != NULL) {
        SDL_SaveBMP(surface, path);
        SDL_FreeSurface(surface);
    }
}
#endif  /* PORT_TEST_HOOKS */

static void ProcessEvents(void)
{
    SDL_Event event;

#ifdef PORT_TEST_HOOKS
    TestHookPoll(Now(), QueueKey, SaveScreenshot);
#endif  /* PORT_TEST_HOOKS */

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            exit(EXIT_SUCCESS);
            break;

        case SDL_KEYDOWN:
            if (
                event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                (event.key.keysym.mod & KMOD_ALT)
            ) {
                if (!event.key.repeat) ToggleFullscreen();
                break;
            }
            QueueKey(event.key.keysym.scancode, 1);
            break;

        case SDL_KEYUP:
            if (
                event.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                (event.key.keysym.mod & KMOD_ALT)
            ) break;
            QueueKey(event.key.keysym.scancode, 0);
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                ReleaseAllKeys();
            }
            break;
        }
    }
}

/*****************************************************************************
 * The main loop substitute                                                  *
 *****************************************************************************/

/*
Bring all emulated hardware up to the current wall-clock time. Returns the
number of timer ticks that elapsed.
*/
static int Pump(void)
{
    double now;
    int ticks = 0;

    if (!isInitialized) PlatformInit();
    if (isInInterrupt) return 0;

    ProcessEvents();
    DeliverKeyboardByte();

    now = Now();
    if (now - nextTickTime > MAX_CATCHUP_S) {
        nextTickTime = now;
    }

    while (now >= nextTickTime) {
        isInInterrupt = 1;
        vectors[8]();
        isInInterrupt = 0;

        GenerateAudio();
        nextTickTime += 1.0 / pitRate;
        ticks++;
    }

    FlushAudio();

    if (now >= nextPresentTime) {
        Present();
        nextPresentTime = now + (1.0 / PRESENT_HZ);
    }

    return ticks;
}

void PortIdle(void)
{
    if (Pump() == 0) {
        SDL_Delay(1);
    }
}

/*****************************************************************************
 * DOS.H                                                                     *
 *****************************************************************************/

void disable(void) {}
void enable(void) {}

void (*getvect(int interruptno))(void)
{
    if (!isInitialized) PlatformInit();

    return vectors[interruptno & 0xff];
}

void setvect(int interruptno, void (*isr)(void))
{
    if (!isInitialized) PlatformInit();

    vectors[interruptno & 0xff] = isr != NULL ? isr : DefaultVector;
}

void outportb(int portid, unsigned char value)
{
    if (!isInitialized) PlatformInit();

    switch (portid) {
    case 0x0020:  /* PIC end-of-interrupt */
        break;

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
        break;

    case 0x0201:  /* joystick one-shot trigger */
        break;

    case 0x0388:
        OPL2WriteAddress(value);
        break;

    case 0x0389:
        OPL2WriteData(value);
        break;

    default:
        EGAPortWrite(portid, value);
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
    if (!isInitialized) PlatformInit();

    switch (portid) {
    case 0x0060:
        /* Polling the keyboard is usually part of a wait loop. */
        if (!isInInterrupt) {
            static int spins = 0;

            if (Pump() == 0 && ++spins > 64) {
                spins = 0;
                SDL_Delay(1);
            }
        }
        return port60;

    case 0x0061:
        return port61;

    case 0x0201:
        /* No joystick: axis timers expire immediately, buttons not pressed */
        return 0xf0;

    case 0x0388:
        return OPL2ReadStatus();

    default:
        return EGAPortRead(portid);
    }
}

int int86(int intno, union REGS *inregs, union REGS *outregs)
{
    if (intno == 0x10 && inregs->h.ah == 0x0f) {
        /* Get Video State */
        outregs->h.al = EGACurrentVideoMode();
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

    if (!isInitialized) return;

    FlushAudio();
    SDL_HideWindow(window);
}

int getch(void)
{
    return getchar();
}

/*
Print an 80x25 page of PC text mode memory (character/attribute pairs) to the
terminal, standing in for the B800h text screen the game writes on exit.
*/
void PortShowTextScreen(const unsigned char *buffer)
{
    static const char *cp437[256] = {
        " ", "☺", "☻", "♥", "♦", "♣", "♠", "•", "◘", "○", "◙", "♂", "♀", "♪", "♫", "☼",
        "►", "◄", "↕", "‼", "¶", "§", "▬", "↨", "↑", "↓", "→", "←", "∟", "↔", "▲", "▼",
        " ", "!", "\"", "#", "$", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
        "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "^", "_",
        "`", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "~", "⌂",
        "Ç", "ü", "é", "â", "ä", "à", "å", "ç", "ê", "ë", "è", "ï", "î", "ì", "Ä", "Å",
        "É", "æ", "Æ", "ô", "ö", "ò", "û", "ù", "ÿ", "Ö", "Ü", "¢", "£", "¥", "₧", "ƒ",
        "á", "í", "ó", "ú", "ñ", "Ñ", "ª", "º", "¿", "⌐", "¬", "½", "¼", "¡", "«", "»",
        "░", "▒", "▓", "│", "┤", "╡", "╢", "╖", "╕", "╣", "║", "╗", "╝", "╜", "╛", "┐",
        "└", "┴", "┬", "├", "─", "┼", "╞", "╟", "╚", "╔", "╩", "╦", "╠", "═", "╬", "╧",
        "╨", "╤", "╥", "╙", "╘", "╒", "╓", "╫", "╪", "┘", "┌", "█", "▄", "▌", "▐", "▀",
        "α", "ß", "Γ", "π", "Σ", "σ", "µ", "τ", "Φ", "Θ", "Ω", "δ", "∞", "φ", "ε", "∩",
        "≡", "±", "≥", "≤", "⌠", "⌡", "÷", "≈", "°", "∙", "·", "√", "ⁿ", "²", "■", " "
    };
    static const uint8_t cga[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0xaa}, {0x00, 0xaa, 0x00}, {0x00, 0xaa, 0xaa},
        {0xaa, 0x00, 0x00}, {0xaa, 0x00, 0xaa}, {0xaa, 0x55, 0x00}, {0xaa, 0xaa, 0xaa},
        {0x55, 0x55, 0x55}, {0x55, 0x55, 0xff}, {0x55, 0xff, 0x55}, {0x55, 0xff, 0xff},
        {0xff, 0x55, 0x55}, {0xff, 0x55, 0xff}, {0xff, 0xff, 0x55}, {0xff, 0xff, 0xff}
    };
    int color = isatty(STDOUT_FILENO);
    int row, col;

    for (row = 0; row < 25; row++) {
        int lastattr = -1;
        int width = 80;

        if (!color) {
            while (width > 0 && buffer[(row * 160) + ((width - 1) * 2)] == ' ') width--;
        }

        for (col = 0; col < width; col++) {
            unsigned char ch = buffer[(row * 160) + (col * 2)];
            unsigned char attr = buffer[(row * 160) + (col * 2) + 1];

            if (color && attr != lastattr) {
                const uint8_t *fg = cga[attr & 0x0f];
                const uint8_t *bg = cga[(attr >> 4) & 0x07];

                printf(
                    "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm",
                    fg[0], fg[1], fg[2], bg[0], bg[1], bg[2]
                );
                lastattr = attr;
            }

            fputs(cp437[ch], stdout);
        }

        if (color) fputs("\033[0m", stdout);
        fputc('\n', stdout);
    }

    fflush(stdout);
}

/*****************************************************************************
 * ALLOC.H, IO.H, MEM.H, STDLIB.H, STRING.H                                  *
 *****************************************************************************/

/*
Every allocation is surrounded by zeroed padding. The original game reads (and
occasionally writes) a little outside of its buffers, which went unnoticed in
DOS's flat, unprotected heap.
*/
void *PortMalloc(size_t size)
{
    uint8_t *block = calloc(1, size + (2 * HEAP_PADDING));

    if (block == NULL) return NULL;

    coreUsed += ((size + 0x17) >> 4) << 4;

    return block + HEAP_PADDING;
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

/*****************************************************************************
 * STDIO.H                                                                   *
 *****************************************************************************/

/*
DOS filenames are case-insensitive, and the game is not consistent about the
case it uses. Find an existing file whose name matches case-insensitively.
Backslashes are treated as path separators. Returns a pointer to a static
buffer holding the best path to use.
*/
static const char *ResolvePath(const char *filename)
{
    static char path[4096];
    char dir[4096];
    const char *base;
    char *slash;
    DIR *dp;
    struct dirent *ent;
    size_t i;

    for (i = 0; filename[i] != '\0' && i < sizeof path - 1; i++) {
        path[i] = filename[i] == '\\' ? '/' : filename[i];
    }
    path[i] = '\0';

    if (access(path, F_OK) == 0) return path;

    strcpy(dir, path);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        base = path + (slash - dir) + 1;
        if (dir[0] == '\0') strcpy(dir, "/");
    } else {
        strcpy(dir, ".");
        base = path;
    }

    dp = opendir(dir);
    if (dp == NULL) return path;

    while ((ent = readdir(dp)) != NULL) {
        if (strcasecmp(ent->d_name, base) == 0) {
            char found[4096];

            if (slash != NULL) {
                snprintf(found, sizeof found, "%s/%s", dir, ent->d_name);
            } else {
                snprintf(found, sizeof found, "%s", ent->d_name);
            }
            strcpy(path, found);
            break;
        }
    }

    closedir(dp);

    return path;
}

FILE *PortFopen(const char *filename, const char *mode)
{
    return fopen(ResolvePath(filename), mode);
}

int PortFclose(FILE *fp)
{
    if (fp == NULL) return EOF;

    return fclose(fp);
}

int PortRemove(const char *filename)
{
    return remove(ResolvePath(filename));
}

int PortGetw(FILE *fp)
{
    int lo = fgetc(fp);
    int hi = fgetc(fp);

    if (lo == EOF || hi == EOF) return EOF;

    return lo | (hi << 8);
}

int PortPutw(int w, FILE *fp)
{
    if (fputc(w & 0xff, fp) == EOF) return EOF;
    if (fputc((w >> 8) & 0xff, fp) == EOF) return EOF;

    return w;
}
