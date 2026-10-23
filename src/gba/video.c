/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE GBA PORT VIDEO                           *
 *                                                                           *
 * The game draws into two virtual 320x200 pages, the same way it drew into  *
 * the two EGA display pages. Every drawing operation in the game is aligned *
 * to the 8x8 tile grid, so a page is stored as 40x200 words, each holding   *
 * one 8-pixel row of a tile at 4 bits per pixel (leftmost pixel in the low  *
 * nibble). The planar tile images from the group files are converted while  *
 * drawing, straight out of cartridge ROM.                                   *
 *                                                                           *
 * The GBA screen is 240x160, so presenting a page means picking part of it: *
 *                                                                           *
 * - Gameplay: the 30x15 tile game window (centered in the page by WINDOWX), *
 *   plus the status bar squeezed to 30x5 tiles by dropping decorative tile  *
 *   columns and the bottom border row.                                      *
 * - A text frame (menu, dialog) up to 30 tiles wide: a crop around it, or   *
 *   if it's taller than 20 tiles, a crop that is squeezed vertically only.  *
 * - Anything else that fits in 30x20 tiles: a crop around what was drawn.   *
 * - Anything larger (full-screen images, wide text pages): the whole page   *
 *   shrunk to 240x150, antialiased with a palette of blended color pairs.   *
 *                                                                           *
 * Bitmap mode 4 gives two 8-bit pages to flip between. Palette entry        *
 * a*16+b holds the average of EGA colors a and b, so entry a*17 is pure a.  *
 * Palette fades rewrite those entries and need no redraw.                   *
 *****************************************************************************/

#include <string.h>

#include <gba_systemcalls.h>

#include "gbaport.h"
#include "../def.h"
#include "../graphics.h"

#define REG_DISPCNT_    (*(volatile uint16_t *)0x04000000)
#define REG_IME_        (*(volatile uint16_t *)0x04000208)
#define PALETTE_RAM     ((volatile uint16_t *)0x05000000)
#define VRAM_PAGE(n)    ((uint16_t *)(0x06000000 + ((n) * 0xa000)))
#define DISPCNT_MODE4   0x0004
#define DISPCNT_PAGE1   0x0010
#define DISPCNT_BG2     0x0400

#define PAGE_WORDS      (200 * 40)
#define CELL_COLS       40
#define CELL_ROWS       25
#define SCREEN_COLS     30
#define SCREEN_ROWS     20
#define SCALED_HEIGHT   150
#define SCALED_TOP      ((160 - SCALED_HEIGHT) / 2)

/* Where the EGA solid tile area began; DrawSolidTile() offsets are relative. */
#define EGA_TILE_BASE   0x4000

enum {MODE_NONE, MODE_GAMEPLAY, MODE_CROP, MODE_VSCALE, MODE_SCALE};

static uint32_t pages[2][PAGE_WORDS] EWRAM_ZERO;
static uint8_t occupied[2][CELL_ROWS * CELL_COLS] EWRAM_ZERO;
static uint8_t frameBorder[2][CELL_ROWS * CELL_COLS] EWRAM_ZERO;  /* cell shows a text frame edge */

static uint32_t spread[256];  /* bit n of a byte -> bit 0 of nibble 7-n */
static uint16_t nibblePair[256];  /* two 4-bit pixels -> two pure palette indices */
static uint16_t scaleX[240];
static uint8_t scaleXWeight[240];
static uint8_t scaleY[SCALED_HEIGHT], scaleYWeight[SCALED_HEIGHT];

static int drawPage, displayPage;
static uint8_t sequencerIndex, mapMask = 0x0f;
static uint8_t paletteRegs[16];
static uint16_t egaRGB[16];

static const uint8_t *solidTiles, *statusTiles, *backdropTiles;

static int isDirty, dirtyX0, dirtyY0, dirtyX1, dirtyY1;
static int forceFull;
static int lastMode = MODE_NONE, lastWindowX, lastWindowY;

static int frontPage;  /* VRAM page being displayed */

static void SetDisplayControl(void);
static volatile int pendingFlip;
static uint16_t *target;  /* VRAM page currently being drawn by Present */

/*
The status bar is 304x48 pixels on the page (tile columns 1-38, rows 19-24),
but only 240x40 fits below the play area. It is rebuilt to fit: each pixel
column and row of the GBA version is one column and row of the original, so
the four groups (score, health, bombs, stars) come through whole, along with
the numbers the game draws into them. What gets left out is plain rock between
the groups, the corner rivets, "PRESS F1 FOR HELP", and some of the frame's
border lines. Spans are {first, count} in status bar pixels.
*/
#define HUD_HEIGHT      ((SCREEN_ROWS - SCROLLH) * 8)
#define STATUS_PAGE_X   8
#define STATUS_PAGE_Y   (19 * 8)

typedef struct {
    uint16_t first, count;
} Span;

static const Span hudColumnSpans[] = {
    {0, 4},                                     /* left edge */
    {11, 1}, {11, 1}, {11, 1}, {11, 1}, {11, 1},
    {12, 63},                                   /* score */
    {77, 6},
    {90, 51},                                   /* health */
    {141, 3}, {148, 2}, {165, 1}, {165, 1}, {165, 1},
    {166, 41},                                  /* bombs */
    {210, 3},
    {232, 50},                                  /* stars */
    {284, 5},
    {300, 4}                                    /* right edge */
};
static const Span hudRowSpans[] = {
    {0, 3},    /* top border */
    {7, 11},   /* labels */
    {19, 24},  /* boxes, star, and bomb */
    {44, 1}, {47, 1}  /* bottom border */
};

/* "PRESS F1 FOR HELP" (the wrong button here), painted over with plain rock */
#define HINT_X0         12
#define HINT_X1         75
#define HINT_Y0         36
#define HINT_Y1         43
#define ROCK_COLOR      6

static uint16_t hudColumns[240];
static uint8_t hudRows[HUD_HEIGHT];
static uint32_t hudCache[HUD_HEIGHT * SCREEN_COLS];  /* packed like the pages */
static int isHudStale = 1;

/*****************************************************************************
 * Helpers                                                                   *
 *****************************************************************************/

/*
Convert one row of a row-planar tile (one byte per plane, blue first) into a
packed 4-bit row.
*/
static inline __attribute__((always_inline)) uint32_t PackRow(const uint8_t *p)
{
    return spread[p[0]] | (spread[p[1]] << 1) | (spread[p[2]] << 2) | (spread[p[3]] << 3);
}

/* A nibble mask: 0xf for every pixel whose mask bit is set (transparent). */
static inline __attribute__((always_inline)) uint32_t MaskRow(uint8_t mask)
{
    return spread[mask] * 0x0f;
}

static inline __attribute__((always_inline)) void Touch(int page, int x, int y, int occ)
{
    if ((unsigned)x >= CELL_COLS || (unsigned)y >= CELL_ROWS) return;

    occupied[page][(y * CELL_COLS) + x] = (uint8_t)occ;
    if (y >= STATUS_PAGE_Y / 8) isHudStale = 1;

    if (page != displayPage) return;

    if (!isDirty) {
        isDirty = 1;
        dirtyX0 = dirtyX1 = x;
        dirtyY0 = dirtyY1 = y;
    } else {
        if (x < dirtyX0) dirtyX0 = x;
        if (x > dirtyX1) dirtyX1 = x;
        if (y < dirtyY0) dirtyY0 = y;
        if (y > dirtyY1) dirtyY1 = y;
    }
}

/*
Fetch the eight packed rows of the tile image at EGA address `ega`. The solid
tile and status bar areas map straight onto their group file data. The three
shifted backdrop copies are made on the fly from the unshifted one; see
WrapBackdropHorizontal() and WrapBackdropVertical() for what they contain.
*/
static inline __attribute__((always_inline)) void TileRows(uint16_t ega, uint32_t *rows)
{
    int r;

    if (ega >= EGA_OFFSET_SOLID_TILES && ega < EGA_OFFSET_SOLID_TILES + 16000U && solidTiles != NULL) {
        const uint8_t *src = solidTiles + ((ega - EGA_OFFSET_SOLID_TILES) * 4);

        for (r = 0; r < 8; r++) rows[r] = PackRow(src + (r * 4));
    } else if (ega >= EGA_OFFSET_STATUS_TILES && ega < EGA_OFFSET_STATUS_TILES + (7296 / 4) && statusTiles != NULL) {
        const uint8_t *src = statusTiles + ((ega - EGA_OFFSET_STATUS_TILES) * 4);

        for (r = 0; r < 8; r++) rows[r] = PackRow(src + (r * 4));
    } else if (ega >= EGA_OFFSET_BDROP_EVEN && backdropTiles != NULL) {
        unsigned offset = ega - EGA_OFFSET_BDROP_EVEN;
        unsigned variant = offset / BACKDROP_SIZE_EGA_MEM;  /* bit 0: odd X, bit 1: odd Y */
        unsigned tile = (offset % BACKDROP_SIZE_EGA_MEM) / 8;
        unsigned tx = tile % BACKDROP_WIDTH, ty = tile / BACKDROP_WIDTH;
        unsigned tx2 = (tx + 1) % BACKDROP_WIDTH;

        for (r = 0; r < 8; r++) {
            unsigned srow = (variant & 2) ? r + 4 : r;
            unsigned sty = (ty + (srow >> 3)) % BACKDROP_HEIGHT;
            const uint8_t *src = backdropTiles + (((sty * BACKDROP_WIDTH) + tx) * 32) + ((srow & 7) * 4);

            if (variant & 1) {
                const uint8_t *next = backdropTiles + (((sty * BACKDROP_WIDTH) + tx2) * 32) + ((srow & 7) * 4);
                uint8_t shifted[4];
                int p;

                for (p = 0; p < 4; p++) {
                    shifted[p] = (uint8_t)((src[p] << 4) | (next[p] >> 4));
                }
                rows[r] = PackRow(shifted);
            } else {
                rows[r] = PackRow(src);
            }
        }
    } else {
        for (r = 0; r < 8; r++) rows[r] = 0;
    }
}

static uint16_t ColorValueToBGR(uint8_t value)
{
    unsigned r = (value & 0x04) ? 0xaa : 0x00;
    unsigned g = (value & 0x02) ? 0xaa : 0x00;
    unsigned b = (value & 0x01) ? 0xaa : 0x00;

    /* Same 200-line mode rules as the SDL port; see SetPaletteRegister. */
    if (value & 0x10) {
        r += 0x55;
        g += 0x55;
        b += 0x55;
    } else if ((value & 0x07) == 0x06) {
        g = 0x55;
    }

    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

static uint16_t Blend(uint16_t a, uint16_t b)
{
    return (uint16_t)(((a & 0x7bde) >> 1) + ((b & 0x7bde) >> 1) + (a & b & 0x0421));
}

static void UpdatePaletteEntry(int index)
{
    int k;

    egaRGB[index] = ColorValueToBGR(paletteRegs[index]);

    for (k = 0; k < 16; k++) {
        uint16_t c = Blend(egaRGB[index], egaRGB[k]);

        PALETTE_RAM[(index * 16) + k] = c;
        PALETTE_RAM[(k * 16) + index] = c;
    }
    PALETTE_RAM[index * 17] = egaRGB[index];
}

/*****************************************************************************
 * Presentation                                                              *
 *****************************************************************************/

/*
Copy `count` pixel rows from the display page into the target, starting at
page row `srcy` and screen row `dsty`, taking tile columns from `columns`.
*/
ARM_IWRAM static void RenderBand(int dsty, int srcy, int count, const uint8_t *columns)
{
    const uint32_t *page = pages[displayPage];
    int y, c;

    for (y = 0; y < count; y++) {
        const uint32_t *src = page + ((srcy + y) * CELL_COLS);
        uint32_t *dst = (uint32_t *)(target + ((dsty + y) * 120));

        for (c = 0; c < SCREEN_COLS; c++) {
            uint32_t w = src[columns[c]];

            dst[0] = nibblePair[w & 0xff] | (nibblePair[(w >> 8) & 0xff] << 16);
            dst[1] = nibblePair[(w >> 16) & 0xff] | (nibblePair[w >> 24] << 16);
            dst += 2;
        }
    }
}

/* Rebuild the fitted status bar from the display page. */
ARM_IWRAM static void RebuildHud(void)
{
    const uint32_t *page = pages[displayPage];
    uint32_t *out = hudCache;
    int y, x, k;

    for (y = 0; y < HUD_HEIGHT; y++) {
        const uint32_t *row = page + (hudRows[y] * CELL_COLS);

        unsigned sy = hudRows[y] - STATUS_PAGE_Y;
        int inhintrows = sy >= HINT_Y0 && sy <= HINT_Y1;

        for (x = 0; x < 240; x += 8) {
            uint32_t w = 0;

            for (k = 0; k < 8; k++) {
                unsigned sx = hudColumns[x + k], c;

                if (inhintrows && sx - STATUS_PAGE_X >= HINT_X0 && sx - STATUS_PAGE_X <= HINT_X1) {
                    c = ROCK_COLOR;
                } else {
                    c = (row[sx >> 3] >> ((sx & 7) * 4)) & 0x0f;
                }
                w |= c << (k * 4);
            }
            *out++ = w;
        }
    }

    isHudStale = 0;
}

/* Copy the fitted status bar into the target, starting at screen row `dsty`. */
ARM_IWRAM static void RenderHud(int dsty)
{
    const uint32_t *src = hudCache;
    uint32_t *dst = (uint32_t *)(target + (dsty * 120));
    int i;

    for (i = 0; i < HUD_HEIGHT * SCREEN_COLS; i++, dst += 2) {
        uint32_t w = src[i];

        dst[0] = nibblePair[w & 0xff] | (nibblePair[(w >> 8) & 0xff] << 16);
        dst[1] = nibblePair[(w >> 16) & 0xff] | (nibblePair[w >> 24] << 16);
    }
}

/*
Like RenderBand() for the whole screen height, but squeezing all 200 page rows
into 160: every 5 page rows become 4, with rows blended where they straddle.
*/
ARM_IWRAM static void RenderSqueezed(int wx)
{
    const uint32_t *page = pages[displayPage] + wx;
    int oy, c, k;

    for (oy = 0; oy < 160; oy++) {
        unsigned sy = ((oy / 4) * 5) + (oy % 4);
        unsigned weight = 4 - (oy % 4);  /* of 5, for row sy; the rest is row sy+1 */
        const uint32_t *ra = page + (sy * CELL_COLS), *rb = ra + CELL_COLS;
        uint32_t *dst = (uint32_t *)(target + (oy * 120));

        for (c = 0; c < SCREEN_COLS; c++, dst += 2) {
            uint32_t a = ra[c], b = rb[c];
            uint8_t out[8];

            if (a == b || weight == 4) {
                dst[0] = nibblePair[a & 0xff] | (nibblePair[(a >> 8) & 0xff] << 16);
                dst[1] = nibblePair[(a >> 16) & 0xff] | (nibblePair[a >> 24] << 16);
                continue;
            }

            for (k = 0; k < 8; k++) {
                unsigned pa = (a >> (k * 4)) & 0x0f, pb = (b >> (k * 4)) & 0x0f;

                if (pa == pb) {
                    out[k] = (uint8_t)(pa * 17);
                } else if (weight == 1) {
                    out[k] = (uint8_t)(pb * 17);
                } else {
                    out[k] = (uint8_t)((pa * 16) + pb);
                }
            }
            dst[0] = out[0] | (out[1] << 8) | (out[2] << 16) | ((uint32_t)out[3] << 24);
            dst[1] = out[4] | (out[5] << 8) | (out[6] << 16) | ((uint32_t)out[7] << 24);
        }
    }
}

static inline __attribute__((always_inline)) unsigned Pixel(const uint32_t *page, unsigned x, unsigned y)
{
    return (page[(y * CELL_COLS) + (x >> 3)] >> ((x & 7) * 4)) & 0x0f;
}

/*
Produce one output pixel of the shrunken page from the 2x2 source pixels it
overlaps. If a second color covers a fifth or more of the area, use the palette
entry that blends the two most prominent colors.
*/
static inline __attribute__((always_inline)) unsigned ScaledPixel(const uint32_t *page, unsigned ox, unsigned oy)
{
    unsigned sx = scaleX[ox], sy = scaleY[oy];
    unsigned wx0 = scaleXWeight[ox], wy0 = scaleYWeight[oy];
    unsigned colors[4], weights[4];
    unsigned n = 0, i, j;
    unsigned best = 0, second = 0;

    colors[0] = Pixel(page, sx, sy);
    colors[1] = Pixel(page, sx + 1, sy);
    colors[2] = Pixel(page, sx, sy + 1);
    colors[3] = Pixel(page, sx + 1, sy + 1);

    if (colors[0] == colors[1] && colors[2] == colors[3] && colors[0] == colors[2]) {
        return colors[0] * 17;
    }

    {
        unsigned w[4];
        unsigned wx1 = 4 - wx0, wy1 = 5 - wy0;

        w[0] = wx0 * wy0;
        w[1] = wx1 * wy0;
        w[2] = wx0 * wy1;
        w[3] = wx1 * wy1;

        for (i = 0; i < 4; i++) {
            for (j = 0; j < n; j++) {
                if (colors[j] == colors[i]) break;
            }
            if (j == n) {
                colors[n] = colors[i];
                weights[n] = 0;
                n++;
            }
            weights[j] += w[i];
        }
    }

    for (i = 1; i < n; i++) {
        if (weights[i] > weights[best]) best = i;
    }
    second = best == 0 ? 1 : 0;
    for (i = 0; i < n; i++) {
        if (i != best && weights[i] > weights[second]) second = i;
    }

    if (n == 1 || weights[second] < 4) return colors[best] * 17;

    return (colors[best] * 16) + colors[second];
}

ARM_IWRAM static void RenderScaled(int ox0, int oy0, int ox1, int oy1)
{
    const uint32_t *page = pages[displayPage];
    int ox, oy;

    ox0 &= ~1;

    for (oy = oy0; oy <= oy1; oy++) {
        uint16_t *dst = target + ((oy + SCALED_TOP) * 120);

        for (ox = ox0; ox <= ox1; ox += 2) {
            dst[ox >> 1] = (uint16_t)(ScaledPixel(page, ox, oy) | (ScaledPixel(page, ox + 1, oy) << 8));
        }
    }
}

static void FillRows(int y, int count, int color)
{
    uint32_t fill = (uint32_t)(color & 0xff) * 0x01010101UL;
    uint32_t *dst = (uint32_t *)(target + (y * 120));
    int i;

    for (i = 0; i < count * 60; i++) dst[i] = fill;
}

static int AnyOccupiedOutsideWindow(const uint8_t *occ)
{
    int x, y;

    for (y = 0; y < 19; y++) {
        for (x = 0; x < CELL_COLS; x++) {
            int inside = y >= WINDOWY && y < WINDOWY + SCROLLH && x >= WINDOWX && x < WINDOWX + SCROLLW;

            if (!inside && occ[(y * CELL_COLS) + x]) return 1;
        }
    }

    return 0;
}

/* Pick a window origin that centers the span a..b in `screen` of `page` cells */
static int CenterWindow(int a, int b, int screen, int page)
{
    int origin = a - ((screen - (b - a + 1)) / 2);

    if (origin < 0) origin = 0;
    if (origin > page - screen) origin = page - screen;

    return origin;
}

static int ChooseMode(int *wx, int *wy)
{
    const uint8_t *occ = occupied[displayPage];
    const uint8_t *border = frameBorder[displayPage];
    int x0 = CELL_COLS, y0 = CELL_ROWS, x1 = -1, y1 = -1;
    int x, y;

    if (isInGame && !AnyOccupiedOutsideWindow(occ)) return MODE_GAMEPLAY;

    /* Text frames get the screen to themselves if they're narrow enough */
    for (y = 0; y < CELL_ROWS; y++) {
        for (x = 0; x < CELL_COLS; x++) {
            if (!border[(y * CELL_COLS) + x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    if (x1 >= 0 && x1 - x0 + 1 <= SCREEN_COLS) {
        int height = y1 - y0 + 1;

        *wx = CenterWindow(x0, x1, SCREEN_COLS, CELL_COLS);
        if (height <= SCREEN_ROWS) {
            *wy = CenterWindow(y0, y1, SCREEN_ROWS, CELL_ROWS);
        } else if (height <= SCREEN_ROWS + 2) {
            /* Frame border rows hold no text; let one or both fall off */
            *wy = y0 + (height - SCREEN_ROWS) / 2;
            if (*wy > CELL_ROWS - SCREEN_ROWS) *wy = CELL_ROWS - SCREEN_ROWS;
        } else {
            *wy = 0;
            return MODE_VSCALE;
        }
        return MODE_CROP;
    }

    x0 = CELL_COLS;
    y0 = CELL_ROWS;
    x1 = y1 = -1;
    for (y = 0; y < CELL_ROWS; y++) {
        for (x = 0; x < CELL_COLS; x++) {
            if (!occ[(y * CELL_COLS) + x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }

    if (x1 < 0) {
        *wx = (CELL_COLS - SCREEN_COLS) / 2;
        *wy = (CELL_ROWS - SCREEN_ROWS) / 2;
        return MODE_CROP;
    }

    if (x1 - x0 + 1 > SCREEN_COLS || y1 - y0 + 1 > SCREEN_ROWS) return MODE_SCALE;

    *wx = CenterWindow(x0, x1, SCREEN_COLS, CELL_COLS);
    *wy = CenterWindow(y0, y1, SCREEN_ROWS, CELL_ROWS);

    return MODE_CROP;
}

static void WaitForFlip(void)
{
    while (pendingFlip) {
        if (REG_IME_ == 0) {
            VideoVBlank();
            break;
        }
        VBlankIntrWait();
    }
}

void VideoPresent(int full)
{
    int mode, wx = 0, wy = 0;

    WaitForFlip();

    mode = ChooseMode(&wx, &wy);
    if (forceFull || mode != lastMode || wx != lastWindowX || wy != lastWindowY) {
        full = 1;
    }
    if (!full && !isDirty) return;

    /* Full redraws go to the hidden page and get flipped in at vblank. */
    target = VRAM_PAGE(full ? !frontPage : frontPage);

    if (mode == MODE_GAMEPLAY) {
        uint8_t columns[SCREEN_COLS];
        int c;

        for (c = 0; c < SCREEN_COLS; c++) columns[c] = (uint8_t)(WINDOWX + c);
        RenderBand(0, WINDOWY * 8, SCROLLH * 8, columns);
        if (isHudStale || lastMode != MODE_GAMEPLAY) RebuildHud();
        RenderHud(SCROLLH * 8);
    } else if (mode == MODE_CROP) {
        uint8_t columns[SCREEN_COLS];
        int c;

        for (c = 0; c < SCREEN_COLS; c++) columns[c] = (uint8_t)(wx + c);
        RenderBand(0, wy * 8, SCREEN_ROWS * 8, columns);
    } else if (mode == MODE_VSCALE) {
        RenderSqueezed(wx);
    } else if (full) {
        FillRows(0, SCALED_TOP, 0);
        RenderScaled(0, 0, 239, SCALED_HEIGHT - 1);
        FillRows(SCALED_TOP + SCALED_HEIGHT, 160 - SCALED_TOP - SCALED_HEIGHT, 0);
    } else if (isDirty) {
        /* Only redo the output pixels that the dirty source cells touch */
        int ox0 = ((dirtyX0 * 8 * 3) / 4) - 1;
        int ox1 = ((((dirtyX1 + 1) * 8 * 3) + 3) / 4) + 1;
        int oy0 = ((dirtyY0 * 8 * 4) / 5) - 1;
        int oy1 = ((((dirtyY1 + 1) * 8 * 4) + 4) / 5) + 1;

        if (ox0 < 0) ox0 = 0;
        if (oy0 < 0) oy0 = 0;
        if (ox1 > 239) ox1 = 239;
        if (oy1 > SCALED_HEIGHT - 1) oy1 = SCALED_HEIGHT - 1;
        RenderScaled(ox0, oy0, ox1, oy1);
    }

    isDirty = 0;
    forceFull = 0;
    lastMode = mode;
    lastWindowX = wx;
    lastWindowY = wy;

    if (full) pendingFlip = 1;
}

int VideoNeedsPresent(void)
{
    return isDirty || forceFull;
}

void VideoRequestFull(void)
{
    forceFull = 1;
}

/* Called from the vblank interrupt */
void VideoVBlank(void)
{
    if (!pendingFlip) return;

    frontPage = !frontPage;
    SetDisplayControl();
    pendingFlip = 0;
}

/*****************************************************************************
 * Overlay drawing                                                           *
 *                                                                           *
 * The on-screen keyboard lives in sprites, not the bitmap, so the game can  *
 * keep drawing underneath it without either one damaging the other. Eight   *
 * 64x32 sprites at 8 bpp fill all of OBJ VRAM and cover a 256x64 canvas,    *
 * placed at the bottom of the screen. The OBJ palette is fixed, so the      *
 * keyboard also stays readable while the game fades its own palette.        *
 *****************************************************************************/

#define OBJ_PALETTE     ((volatile uint16_t *)0x05000200)
#define OBJ_VRAM        ((uint16_t *)0x06014000)
#define OAM             ((volatile uint16_t *)0x07000000)
#define DISPCNT_OBJ     0x1000
#define DISPCNT_OBJ_1D  0x0040
#define OVERLAY_W       256
#define OVERLAY_H       64

static int isOverlayShown;

/* Byte offset of a canvas pixel: 4x2 sprites of 8x4 tiles, 64 bytes each */
static inline uint32_t OverlayPixel(int x, int y)
{
    uint32_t sprite = ((uint32_t)(y / 32) * 4) + (uint32_t)(x / 64);
    uint32_t tile = (sprite * 32) + ((uint32_t)((y % 32) / 8) * 8) + (uint32_t)((x % 64) / 8);

    return (tile * 64) + ((uint32_t)(y % 8) * 8) + (uint32_t)(x % 8);
}

static void SetDisplayControl(void)
{
    REG_DISPCNT_ = (uint16_t)(
        DISPCNT_MODE4 | DISPCNT_BG2 | DISPCNT_OBJ_1D | (frontPage ? DISPCNT_PAGE1 : 0) |
        (isOverlayShown ? DISPCNT_OBJ : 0)
    );
}

static void OverlayInit(void)
{
    static const uint16_t colors[OVERLAY_COLORS] = {
        0x0000, 0x0000, 0x2108, 0x5000, 0x2bff, 0x7fff
    };
    int i;

    for (i = 0; i < OVERLAY_COLORS; i++) OBJ_PALETTE[i] = colors[i];

    for (i = 0; i < 128; i++) {
        if (i < 8) {
            /* Wide shape, 8 bpp; size 3 makes that 64x32 */
            OAM[(i * 4) + 0] = (uint16_t)(0x6000 | (160 - OVERLAY_HEIGHT + ((i / 4) * 32)));
            OAM[(i * 4) + 1] = (uint16_t)(0xc000 | ((i % 4) * 64));
            OAM[(i * 4) + 2] = (uint16_t)(512 + (i * 64));
        } else {
            OAM[(i * 4) + 0] = 0x0200;  /* hidden */
        }
    }
}

void VideoShowOverlay(int show)
{
    isOverlayShown = show;
    SetDisplayControl();
}

void VideoFillRect(int x, int y, int w, int h, int color)
{
    uint16_t fill = (uint16_t)((color & 0xff) * 0x0101);
    int i, j;

    for (j = y; j < y + h; j++) {
        if (j < 0 || j >= OVERLAY_H) continue;
        for (i = x & ~1; i < x + w; i += 2) {
            if (i < 0 || i >= OVERLAY_W) continue;
            OBJ_VRAM[OverlayPixel(i, j) / 2] = fill;
        }
    }
}

/*
Draw text with the game's own font, on the overlay canvas. `x` must be even.
The font's white pixels get `fg`; everything else (shadows, the gray behind
the digits, transparency) gets `bg`.
*/
void VideoDrawText(int x, int y, const char *text, int fg, int bg)
{
    for (; *text != '\0'; text++, x += 8) {
        unsigned char ch = (unsigned char)*text;
        const uint8_t *glyph;
        int row, col;

        if (fontTileData == NULL || x + 8 > OVERLAY_W) return;

        if (ch >= 'a') {
            glyph = fontTileData + FONT_LOWER_A + ((ch - 'a') * 40);
        } else {
            glyph = fontTileData + FONT_UP_ARROW + ((ch - '\x18') * 40);
        }

        for (row = 0; row < 8; row++) {
            const uint8_t *s = glyph + (row * 5);
            uint8_t white = (uint8_t)(~s[0] & s[1] & s[2] & s[3] & s[4]);

            if (y + row < 0 || y + row >= OVERLAY_H) continue;

            for (col = 0; col < 8; col += 2) {
                int left = (white >> (7 - col)) & 1 ? fg : bg;
                int right = (white >> (6 - col)) & 1 ? fg : bg;

                OBJ_VRAM[OverlayPixel(x + col, y + row) / 2] =
                    (uint16_t)((left & 0xff) | ((right & 0xff) << 8));
            }
        }
    }
}

/*****************************************************************************
 * Setup and EGA port access                                                 *
 *****************************************************************************/

void VideoInit(void)
{
    int i, b;
    static const uint8_t xOffset[3] = {0, 1, 2}, xWeight[3] = {3, 2, 1};

    for (i = 0; i < 256; i++) {
        uint32_t s = 0;

        for (b = 0; b < 8; b++) {
            if (i & (0x80 >> b)) s |= 1UL << (b * 4);
        }
        spread[i] = s;
        nibblePair[i] = (uint16_t)(((i & 0x0f) * 17) | (((i >> 4) * 17) << 8));
    }

    /* 4 source pixels -> 3 across, 5 source rows -> 4 down */
    for (i = 0; i < 240; i++) {
        scaleX[i] = (uint16_t)(((i / 3) * 4) + xOffset[i % 3]);
        scaleXWeight[i] = xWeight[i % 3];
    }
    for (i = 0; i < SCALED_HEIGHT; i++) {
        scaleY[i] = (uint8_t)(((i / 4) * 5) + (i % 4));
        scaleYWeight[i] = (uint8_t)(4 - (i % 4));
    }

    for (i = 0; i < 16; i++) {
        paletteRegs[i] = (uint8_t)(i < 8 ? i : i + 8);
    }

    for (i = 0, b = 0; i < (int)(sizeof hudColumnSpans / sizeof hudColumnSpans[0]); i++) {
        int k;

        for (k = 0; k < hudColumnSpans[i].count && b < 240; k++) {
            hudColumns[b++] = (uint16_t)(STATUS_PAGE_X + hudColumnSpans[i].first + k);
        }
    }
    for (i = 0, b = 0; i < (int)(sizeof hudRowSpans / sizeof hudRowSpans[0]); i++) {
        int k;

        for (k = 0; k < hudRowSpans[i].count && b < HUD_HEIGHT; k++) {
            hudRows[b++] = (uint8_t)(STATUS_PAGE_Y + hudRowSpans[i].first + k);
        }
    }
    for (i = 0; i < 16; i++) UpdatePaletteEntry(i);

    memset(VRAM_PAGE(0), 0, 2 * 0xa000);
    frontPage = 0;
    OverlayInit();
    SetDisplayControl();
}

void VideoPortWrite(int port, uint8_t value)
{
    if (port == 0x03c4) {
        sequencerIndex = value;
    } else if (port == 0x03c5 && sequencerIndex == 0x02) {
        mapMask = value & 0x0f;
    }
}

/*
Write one byte into one or more planes (per the map mask) of the display page
area, the way the game writes full-screen images.
*/
void EGAWriteByte(unsigned short offset, unsigned char value)
{
    uint32_t *dst;
    unsigned page, off;
    int p;

    if (offset >= EGA_TILE_BASE) return;

    page = offset >> 13;
    off = offset & 0x1fff;
    if (off >= PAGE_WORDS) return;

    dst = &pages[page][off];
    for (p = 0; p < 4; p++) {
        if (mapMask & (1 << p)) {
            *dst = (*dst & ~(0x11111111UL << p)) | (spread[value] << p);
        }
    }

    Touch(page, off % CELL_COLS, off / (CELL_COLS * 8), 1);
    frameBorder[page][((off / (CELL_COLS * 8)) * CELL_COLS) + (off % CELL_COLS)] = 0;
}

void PortCopyTilesToEGA(unsigned char *source, unsigned short length, unsigned short offset)
{
    (void) length;

    switch (offset) {
    case EGA_OFFSET_SOLID_TILES:
        solidTiles = source;
        break;
    case EGA_OFFSET_STATUS_TILES:
        statusTiles = source;
        break;
    case EGA_OFFSET_BDROP_EVEN:
        backdropTiles = source;
        break;
    }
}

/*
The game window is narrower than on DOS, so text frames drawn during gameplay
can leave remnants beside it. Clear those as each new frame starts.
*/
void PortBeginMapRegion(void)
{
    uint8_t *occ = occupied[drawPage];
    int x, y, r;

    for (y = 0; y < 19; y++) {
        for (x = 0; x < CELL_COLS; x++) {
            int inside = y >= WINDOWY && y < WINDOWY + SCROLLH && x >= WINDOWX && x < WINDOWX + SCROLLW;
            uint32_t *dst;

            if (inside || !occ[(y * CELL_COLS) + x]) continue;

            dst = &pages[drawPage][(y * 8 * CELL_COLS) + x];
            for (r = 0; r < 8; r++) dst[r * CELL_COLS] = 0;
            Touch(drawPage, x, y, 0);
            frameBorder[drawPage][(y * CELL_COLS) + x] = 0;
        }
    }
}

/*****************************************************************************
 * Equivalents of every public procedure in lowlevel.asm                     *
 *****************************************************************************/

void SetVideoMode(word mode_num)
{
    int i;

    if ((mode_num & 0x80) == 0) {
        memset(pages, 0, sizeof pages);
        memset(occupied, 0, sizeof occupied);
        memset(frameBorder, 0, sizeof frameBorder);
    }
    for (i = 0; i < 16; i++) {
        paletteRegs[i] = (uint8_t)(i < 8 ? i : i + 8);
        UpdatePaletteEntry(i);
    }
    drawPage = displayPage = 0;
    mapMask = 0x0f;
    forceFull = 1;
}

void SetBorderColorRegister(word color_value)
{
    (void) color_value;
}

void SetPaletteRegister(word palette_index, word color_value)
{
    paletteRegs[palette_index & 0x0f] = (uint8_t)color_value;
    UpdatePaletteEntry(palette_index & 0x0f);
}

ARM_IWRAM void DrawSolidTile(word src_offset, word dst_offset)
{
    uint16_t ega = (uint16_t)(EGA_TILE_BASE + src_offset);
    unsigned row = dst_offset / CELL_COLS, col = dst_offset % CELL_COLS;
    uint32_t rows[8];
    uint32_t *dst;
    int r;

    if (row > 192) return;

    TileRows(ega, rows);

    dst = &pages[drawPage][(row * CELL_COLS) + col];
    for (r = 0; r < 8; r++) dst[r * CELL_COLS] = rows[r];

    Touch(drawPage, col, row / 8, ega != EGA_TILE_BASE);
    if (row / 8 < CELL_ROWS) {
        frameBorder[drawPage][((row / 8) * CELL_COLS) + col] = (
            src_offset >= TILE_TXTFRAME_NORTHWEST && src_offset <= TILE_TXTFRAME_EAST
        );
    }
}

void SelectDrawPage(word page_num)
{
    drawPage = page_num & 1;
}

void SelectActivePage(word page_num)
{
    displayPage = page_num & 1;
    VideoPresent(1);
}

static inline __attribute__((always_inline)) uint32_t *CellAddress(word x, word y)
{
    if (x >= CELL_COLS || y >= CELL_ROWS) return NULL;

    return &pages[drawPage][(y * 8 * CELL_COLS) + x];
}

ARM_IWRAM void DrawSpriteTile(byte *src, word x, word y)
{
    uint32_t *dst = CellAddress(x, y);
    int r;

    if (dst == NULL) return;

    for (r = 0; r < 8; r++, src += 5) {
        dst[r * CELL_COLS] = (dst[r * CELL_COLS] & MaskRow(src[0])) | PackRow(src + 1);
    }

    Touch(drawPage, x, y, 1);
}

ARM_IWRAM void DrawMaskedTile(byte *src, word x, word y)
{
    DrawSpriteTile(src - 16000, x, y);
}

ARM_IWRAM void DrawSpriteTileFlipped(byte *src, word x, word y)
{
    uint32_t *dst = CellAddress(x, y);
    int r;

    if (dst == NULL) return;

    for (r = 0; r < 8; r++, src += 5) {
        uint32_t *d = dst + ((7 - r) * CELL_COLS);

        *d = (*d & MaskRow(src[0])) | PackRow(src + 1);
    }

    Touch(drawPage, x, y, 1);
}

ARM_IWRAM void DrawSpriteTileWhite(byte *src, word x, word y)
{
    uint32_t *dst = CellAddress(x, y);
    int r;

    if (dst == NULL) return;

    for (r = 0; r < 8; r++, src += 5) {
        dst[r * CELL_COLS] |= ~MaskRow(src[0]);
    }

    Touch(drawPage, x, y, 1);
}

ARM_IWRAM void DrawSpriteTileTranslucent(byte *src, word x, word y)
{
    uint32_t *dst = CellAddress(x, y);
    int r;

    if (dst == NULL) return;

    for (r = 0; r < 8; r++, src += 5) {
        dst[r * CELL_COLS] |= ~MaskRow(src[0]) & 0x88888888UL;
    }

    Touch(drawPage, x, y, 1);
}

static void LightenWithMasks(word x, word y, const uint8_t *masks)
{
    uint32_t *dst = CellAddress(x, y);
    int r;

    if (dst == NULL) return;

    for (r = 0; r < 8; r++) {
        dst[r * CELL_COLS] |= spread[masks[r]] << 3;
    }

    Touch(drawPage, x, y, 1);
}

void LightenScreenTileWest(word x, word y)
{
    static const uint8_t masks[8] = {0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff};

    LightenWithMasks(x, y, masks);
}

void LightenScreenTile(word x, word y)
{
    static const uint8_t masks[8] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    LightenWithMasks(x, y, masks);
}

void LightenScreenTileEast(word x, word y)
{
    static const uint8_t masks[8] = {0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff};

    LightenWithMasks(x, y, masks);
}

word GetProcessorType(void)
{
    return 7;  /* CPU_TYPE_80386, or close enough */
}
