/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                        COSMORE SDL PORT EGA EMULATION                     *
 *                                                                           *
 * A small model of the EGA's planar memory and the handful of Sequencer and *
 * Graphics Controller registers the game programs, followed by C versions   *
 * of every procedure in lowlevel.asm. The procedures go through the same    *
 * register writes and latched memory reads/writes as the assembly does, so  *
 * the drawing quirks (including the "unsafe assumptions" documented there)  *
 * behave identically.                                                       *
 *****************************************************************************/

#include <string.h>

#include "ega.h"

#define PLANE_SIZE 0x10000L

#define SEQUENCER_ADDR      0x03c4
#define SEQUENCER_DATA      0x03c5
#define GRAPHICS_1_2_ADDR   0x03ce
#define GRAPHICS_DATA       0x03cf
#define SEQ_MAP_MASK        0x02
#define GFX_DATA_ROTATE     0x03
#define GFX_READ_MAP_SELECT 0x04
#define GFX_MODE            0x05
#define GFX_COLOR_DONT_CARE 0x07
#define GFX_BIT_MASK        0x08

#define SCREEN_Y_STRIDE         40
#define MASKED_TILE_ROW_STRIDE  5
#define SOLID_TILE_OFFSET       0x4000
#define PAGE_SIZE               0x2000

extern uint16_t yOffsetTable[];  /* defined in game2.c */

static uint8_t planes[4][PLANE_SIZE];
static uint8_t latches[4];

static uint8_t seqIndex, mapMask = 0x0f;
static uint8_t gcIndex, setReset, enableSetReset, colorCompare, dataRotate;
static uint8_t readMapSelect, gcMode, colorDontCare = 0x0f, bitMask = 0xff;

static uint8_t videoMode = 0x03;
static uint8_t paletteRegs[16];
static uint8_t borderColor;
static uint16_t displayStart;

static uint16_t drawPageNumber;
static uint16_t drawPageOffset;

static void WriteSequencer(uint8_t index, uint8_t value)
{
    switch (index) {
    case SEQ_MAP_MASK:
        mapMask = value & 0x0f;
        break;
    }
}

static void WriteGraphics(uint8_t index, uint8_t value)
{
    switch (index) {
    case 0x00: setReset = value & 0x0f; break;
    case 0x01: enableSetReset = value & 0x0f; break;
    case 0x02: colorCompare = value & 0x0f; break;
    case GFX_DATA_ROTATE: dataRotate = value; break;
    case GFX_READ_MAP_SELECT: readMapSelect = value & 0x03; break;
    case GFX_MODE: gcMode = value; break;
    case GFX_COLOR_DONT_CARE: colorDontCare = value & 0x0f; break;
    case GFX_BIT_MASK: bitMask = value; break;
    }
}

void EGAPortWrite(int port, uint8_t value)
{
    switch (port) {
    case SEQUENCER_ADDR: seqIndex = value; break;
    case SEQUENCER_DATA: WriteSequencer(seqIndex, value); break;
    case GRAPHICS_1_2_ADDR: gcIndex = value; break;
    case GRAPHICS_DATA: WriteGraphics(gcIndex, value); break;
    }
}

uint8_t EGAPortRead(int port)
{
    (void) port;

    return 0xff;
}

uint8_t EGACurrentVideoMode(void)
{
    return videoMode;
}

/*
CPU read from the EGA window. Loads all four latches, then returns data as
selected by the Read Mode.
*/
static uint8_t MemRead(uint16_t offset)
{
    int p;

    for (p = 0; p < 4; p++) {
        latches[p] = planes[p][offset];
    }

    if ((gcMode & 0x08) == 0) {
        return latches[readMapSelect];
    } else {
        uint8_t result = 0xff;

        for (p = 0; p < 4; p++) {
            if (colorDontCare & (1 << p)) {
                uint8_t want = (colorCompare & (1 << p)) ? 0xff : 0x00;
                result &= ~(latches[p] ^ want);
            }
        }

        return result;
    }
}

/*
CPU write to the EGA window, honoring the map mask, write mode, set/reset,
function select, and bit mask.
*/
static void MemWrite(uint16_t offset, uint8_t value)
{
    int p;
    uint8_t rotate = dataRotate & 0x07;

    if (rotate != 0) {
        value = (uint8_t)((value >> rotate) | (value << (8 - rotate)));
    }

    for (p = 0; p < 4; p++) {
        uint8_t data;

        if ((mapMask & (1 << p)) == 0) continue;

        switch (gcMode & 0x03) {
        case 0:
            data = value;
            if (enableSetReset & (1 << p)) {
                data = (setReset & (1 << p)) ? 0xff : 0x00;
            }
            break;
        case 1:
            planes[p][offset] = latches[p];
            continue;
        case 2:
            data = (value & (1 << p)) ? 0xff : 0x00;
            break;
        default:
            continue;
        }

        switch ((dataRotate >> 3) & 0x03) {
        case 1: data &= latches[p]; break;
        case 2: data |= latches[p]; break;
        case 3: data ^= latches[p]; break;
        }

        planes[p][offset] = (data & bitMask) | (latches[p] & ~bitMask);
    }
}

void EGAWriteByte(unsigned short offset, unsigned char value)
{
    MemWrite(offset, value);
}

/*
Convert a 6-bit palette register value into an RGB triple. In the 200-line
modes, only the R/G/B bits and the bit 4 intensity signal matter, and the
display "fudges" dark yellow into brown. See SetPaletteRegister in
lowlevel.asm for the gory details.
*/
static uint32_t ColorValueToRGB(uint8_t value)
{
    uint32_t r = (value & 0x04) ? 0xaa : 0x00;
    uint32_t g = (value & 0x02) ? 0xaa : 0x00;
    uint32_t b = (value & 0x01) ? 0xaa : 0x00;

    if (value & 0x10) {
        r += 0x55;
        g += 0x55;
        b += 0x55;
    } else if ((value & 0x07) == 0x06) {
        g = 0x55;
    }

    return (r << 16) | (g << 8) | b;
}

int EGARenderFrame(uint32_t *pixels)
{
    uint32_t colors[16];
    int i, x, y;

    for (i = 0; i < 16; i++) {
        colors[i] = ColorValueToRGB(paletteRegs[i]);
    }

    if (videoMode != 0x0d) {
        uint32_t border = ColorValueToRGB(borderColor);

        for (i = 0; i < 320 * 200; i++) {
            pixels[i] = border;
        }

        return 1;
    }

    for (y = 0; y < 200; y++) {
        uint16_t rowoff = (uint16_t)(displayStart + (y * SCREEN_Y_STRIDE));

        for (x = 0; x < SCREEN_Y_STRIDE; x++) {
            uint16_t off = (uint16_t)(rowoff + x);
            uint8_t p0 = planes[0][off], p1 = planes[1][off];
            uint8_t p2 = planes[2][off], p3 = planes[3][off];
            int bit;

            for (bit = 7; bit >= 0; bit--) {
                int index =
                    ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) |
                    (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);

                *pixels++ = colors[index];
            }
        }
    }

    return 1;
}

/*****************************************************************************
 * Equivalents of every public procedure in lowlevel.asm                     *
 *****************************************************************************/

#define OUT_WORD(port, value) { \
    EGAPortWrite((port), (uint8_t)((value) & 0xff)); \
    EGAPortWrite((port) + 1, (uint8_t)((value) >> 8)); \
}
#define SELECT_EGA_SEQ_MAP_MASK()       { EGAPortWrite(SEQUENCER_ADDR, SEQ_MAP_MASK); }
#define SET_EGA_COLOR_DONT_CARE(mask)   OUT_WORD(GRAPHICS_1_2_ADDR, ((mask) << 8) | GFX_COLOR_DONT_CARE)
#define SET_EGA_BIT_MASK(mask)          OUT_WORD(GRAPHICS_1_2_ADDR, ((mask) << 8) | GFX_BIT_MASK)
#define SET_EGA_MAP_MASK(mask)          OUT_WORD(SEQUENCER_ADDR, ((mask) << 8) | SEQ_MAP_MASK)
#define SET_EGA_READ_MAP(map)           OUT_WORD(GRAPHICS_1_2_ADDR, ((map) << 8) | GFX_READ_MAP_SELECT)
#define SET_EGA_GC_MODE(mode)           OUT_WORD(GRAPHICS_1_2_ADDR, ((mode) << 8) | GFX_MODE)

static uint16_t ScreenOffset(uint16_t x, uint16_t y)
{
    return (uint16_t)(yOffsetTable[y] + x);
}

void SetVideoMode(uint16_t mode_num)
{
    int i;

    /* What the BIOS would do: reset the hardware and clear video memory. */
    videoMode = (uint8_t)(mode_num & 0x7f);
    if ((mode_num & 0x80) == 0) {
        memset(planes, 0, sizeof planes);
    }
    for (i = 0; i < 16; i++) {
        paletteRegs[i] = (uint8_t)(i < 8 ? i : i + 8);
    }
    borderColor = 0;
    displayStart = 0;
    mapMask = 0x0f;
    setReset = enableSetReset = colorCompare = dataRotate = 0;
    readMapSelect = gcMode = 0;
    colorDontCare = 0x0f;
    bitMask = 0xff;

    SET_EGA_COLOR_DONT_CARE(0x00);
    SELECT_EGA_SEQ_MAP_MASK();
}

void SetBorderColorRegister(uint16_t color_value)
{
    borderColor = (uint8_t)color_value;
}

void SetPaletteRegister(uint16_t palette_index, uint16_t color_value)
{
    paletteRegs[palette_index & 0x0f] = (uint8_t)color_value;
}

void DrawSolidTile(uint16_t src_offset, uint16_t dst_offset)
{
    uint16_t src = (uint16_t)(SOLID_TILE_OFFSET + src_offset);
    uint16_t dst = (uint16_t)(drawPageOffset + dst_offset);
    int row;

    for (row = 0; row < 8; row++) {
        uint8_t al = MemRead((uint16_t)(src + row));
        MemWrite((uint16_t)(dst + (row * SCREEN_Y_STRIDE)), al);
    }
}

void SelectDrawPage(uint16_t page_num)
{
    drawPageNumber = page_num;
    drawPageOffset = (uint16_t)(drawPageNumber * PAGE_SIZE);
}

void DrawSpriteTileTranslucent(uint8_t *src, uint16_t x, uint16_t y)
{
    uint16_t dst = (uint16_t)(drawPageOffset + (y * 320) + x);
    int row;

    SELECT_EGA_SEQ_MAP_MASK();

    for (row = 0; row < 8; row++) {
        uint8_t mask = (uint8_t)~*src;

        OUT_WORD(GRAPHICS_1_2_ADDR, (mask << 8) | GFX_BIT_MASK);
        EGAPortWrite(SEQUENCER_DATA, 0x08);

        src += MASKED_TILE_ROW_STRIDE;
        MemRead(dst);
        MemWrite(dst, 0xff);

        dst += SCREEN_Y_STRIDE;
    }
}

static void LightenWithMasks(uint16_t x, uint16_t y, const uint8_t *masks)
{
    uint16_t dst = (uint16_t)(drawPageOffset + ScreenOffset(x, y));
    int row;

    for (row = 0; row < 8; row++) {
        SET_EGA_BIT_MASK(masks[row]);
        MemRead(dst);
        MemWrite(dst, masks[row]);
        dst += SCREEN_Y_STRIDE;
    }
}

void LightenScreenTileWest(uint16_t x, uint16_t y)
{
    static const uint8_t masks[8] = {
        0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff
    };

    /* Assumes the sequencer index is already pointing at the map mask */
    EGAPortWrite(SEQUENCER_DATA, 0x08);

    LightenWithMasks(x, y, masks);
}

void LightenScreenTile(uint16_t x, uint16_t y)
{
    uint16_t dst = (uint16_t)(drawPageOffset + ScreenOffset(x, y));
    int row;

    SET_EGA_BIT_MASK(0xff);

    /* Assumes the sequencer index is already pointing at the map mask */
    EGAPortWrite(SEQUENCER_DATA, 0x08);

    for (row = 0; row < 8; row++) {
        MemWrite(dst, 0xff);
        dst += SCREEN_Y_STRIDE;
    }
}

void LightenScreenTileEast(uint16_t x, uint16_t y)
{
    static const uint8_t masks[8] = {
        0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff
    };

    /* Assumes the map mask already holds the intensity plane only */
    SELECT_EGA_SEQ_MAP_MASK();

    LightenWithMasks(x, y, masks);
}

void SelectActivePage(uint16_t page_num)
{
    displayStart = (uint16_t)((page_num & 0xff) * PAGE_SIZE);
}

/*
Shared body of DrawSpriteTile and DrawMaskedTile: AND each row with the tile's
mask plane, then OR the color plane in, one EGA plane at a time.
*/
static void DrawMaskedRows(const uint8_t *src, uint16_t dst)
{
    int plane, row;

    for (plane = 0; plane < 4; plane++) {
        SET_EGA_MAP_MASK(1 << plane);
        SET_EGA_READ_MAP(plane);

        for (row = 0; row < 8; row++) {
            uint16_t d = (uint16_t)(dst + (row * SCREEN_Y_STRIDE));
            const uint8_t *s = src + (row * MASKED_TILE_ROW_STRIDE);
            uint8_t al = MemRead(d);

            al &= s[0];
            al |= s[plane + 1];
            MemWrite(d, al);
        }
    }
}

void DrawSpriteTile(uint8_t *src, uint16_t x, uint16_t y)
{
    DrawMaskedRows(src, (uint16_t)(drawPageOffset + ScreenOffset(x, y)));
}

void DrawMaskedTile(uint8_t *src, uint16_t x, uint16_t y)
{
    SET_EGA_GC_MODE(0x00);

    DrawMaskedRows(src - 16000, (uint16_t)(drawPageOffset + ScreenOffset(x, y)));

    SET_EGA_MAP_MASK(0x0f);
    SET_EGA_GC_MODE(0x01);
}

void DrawSpriteTileFlipped(uint8_t *src, uint16_t x, uint16_t y)
{
    uint16_t dst = (uint16_t)(drawPageOffset + ScreenOffset(x, y));
    int plane, row;

    for (plane = 0; plane < 4; plane++) {
        SET_EGA_MAP_MASK(1 << plane);
        SET_EGA_READ_MAP(plane);

        for (row = 0; row < 8; row++) {
            uint16_t d = (uint16_t)(dst + ((7 - row) * SCREEN_Y_STRIDE));
            const uint8_t *s = src + (row * MASKED_TILE_ROW_STRIDE);
            uint8_t al = MemRead(d);

            al &= s[0];
            al |= s[plane + 1];
            MemWrite(d, al);
        }
    }
}

void DrawSpriteTileWhite(uint8_t *src, uint16_t x, uint16_t y)
{
    uint16_t dst = (uint16_t)(drawPageOffset + ScreenOffset(x, y));
    int row;

    /* Assumes the sequencer index is already pointing at the map mask */
    EGAPortWrite(SEQUENCER_DATA, 0x0f);

    OUT_WORD(GRAPHICS_1_2_ADDR, (0x10 << 8) | GFX_DATA_ROTATE);
    OUT_WORD(GRAPHICS_1_2_ADDR, (0x08 << 8) | GFX_MODE);

    for (row = 0; row < 8; row++) {
        uint8_t al = (uint8_t)~src[row * MASKED_TILE_ROW_STRIDE];
        uint8_t mem = MemRead(dst);

        MemWrite(dst, mem & al);
        dst += SCREEN_Y_STRIDE;
    }

    OUT_WORD(GRAPHICS_1_2_ADDR, (0x00 << 8) | GFX_DATA_ROTATE);
    OUT_WORD(GRAPHICS_1_2_ADDR, (0x00 << 8) | GFX_MODE);
}

uint16_t GetProcessorType(void)
{
    return 7;  /* CPU_TYPE_80386, or close enough */
}
