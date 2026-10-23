/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE GBA PORT MENUS                           *
 *                                                                           *
 * The DOS menus are picked from by typing a letter, which on a GBA means    *
 * going through the on-screen keyboard. These replacements for the main     *
 * menu, the in-game help menu, and the "game number (1-9)" prompts are      *
 * driven by the D-pad instead. They are drawn with the game's own frames,   *
 * font, and wait spinner, and they hand back the same scancode the original *
 * prompt would have read, so the game's code for each choice is unchanged.  *
 * The letter keys still work, too.                                          *
 *                                                                           *
 * The buttons arrive as the keys they are mapped to: the D-pad as the arrow *
 * keys, A as Ctrl, B as Alt, and Start as Enter.                            *
 *****************************************************************************/

#include "../glue.h"

#define DRAW_SOLID_TILE_XY(src, x, y) { DrawSolidTile((src), (x) + ((y) * 320)); }

/* Font glyphs, relative to `FONT_UP_ARROW` which is CP437 character 18h */
#define FONT_DOWN_ARROW     (FONT_UP_ARROW + (1 * 40))
#define FONT_SOLID_BLACK    (FONT_UP_ARROW + (2 * 40))

typedef struct {
    char *label;
    byte scancode;
} MenuItem;

static MenuItem mainMenuItems[] = {
    {"New Game",     SCANCODE_B},
    {"Restore Game", SCANCODE_R},
    {"Story",        SCANCODE_S},
    {"Instructions", SCANCODE_I},
    {"High Scores",  SCANCODE_H},
    {"Demo",         SCANCODE_D},
    {"Credits",      SCANCODE_C},
    {"Title Screen", SCANCODE_T}
};
#define MAIN_MENU_COUNT ((int)(sizeof mainMenuItems / sizeof mainMenuItems[0]))
#define MAIN_MENU_TOP   6
#define MAIN_MENU_WIDTH 20

static MenuItem helpMenuItems[] = {
    {"Save Game",    SCANCODE_S},
    {"Restore Game", SCANCODE_R},
    {"Help",         SCANCODE_H},
    {"High Scores",  SCANCODE_V},
    {"Quit Game",    SCANCODE_Q}
};
#define HELP_MENU_COUNT ((int)(sizeof helpMenuItems / sizeof helpMenuItems[0]))
#define HELP_MENU_TOP   2
#define HELP_MENU_WIDTH 22

static word mainMenuX, helpMenuX;
static int mainMenuCursor, helpMenuCursor;
static int lastGameNumber = 1;

/*
Draw one row of a menu, `width` tiles wide. The selected row gets a black bar
behind its label.
*/
static void DrawMenuRow(word x, word y, word width, MenuItem *item, bbool selected)
{
    word i;

    if (selected) {
        EGA_MODE_DEFAULT();
        for (i = 0; i < width; i++) {
            DrawSpriteTile(fontTileData + FONT_SOLID_BLACK, x + i, y);
        }
    } else {
        EGA_MODE_LATCHED_WRITE();
        for (i = 0; i < width; i++) {
            DRAW_SOLID_TILE_XY(TILE_DARK_GRAY, x + i, y);
        }
    }

    DrawTextLine(x + 1, y, item->label);
}

/*
Unfold a menu frame `width` tiles wide at `top`, and draw the items into it with
`cursor` selected. The items start on the third line of the frame. Returns the
X coordinate of the inside-left edge of the frame.
*/
static word DrawMenu(
    int top, int width, char *top_text, char *bottom_text, MenuItem *items, int count,
    int cursor
) {
    int i;
    /* Border, title, blank line, items, blank line, (bottom text,) border */
    int height = count + (*bottom_text != '\0' ? 6 : 5);
    word x = UnfoldTextFrame(top, height, width, top_text, bottom_text);

    for (i = 0; i < count; i++) {
        DrawMenuRow(x, top + 3 + i, width - 2, &items[i], i == cursor);
    }

    return x;
}

/*
Move the selection with the D-pad until A or Start picks an item, and return
that item's scancode. B returns `back_scancode`. Typing an item's own key picks
it directly.
*/
static byte SelectFromMenu(
    word x, word top, word width, MenuItem *items, int count, int *cursor,
    byte back_scancode
) {
    for (;;) {
        word y = top + 3 + *cursor;
        byte scancode = WaitSpinner(x + width - 3, y);
        int i, next = *cursor;

        /* The spinner leaves a gray hole in the bar */
        DrawMenuRow(x, y, width - 2, &items[*cursor], true);

        switch (scancode) {
        case SCANCODE_KP_8:
            next = (*cursor + count - 1) % count;
            break;
        case SCANCODE_KP_2:
            next = (*cursor + 1) % count;
            break;
        case SCANCODE_CTRL:
        case SCANCODE_ENTER:
        case SCANCODE_SPACE:
            return items[*cursor].scancode;
        case SCANCODE_ALT:
        case SCANCODE_ESC:
            return back_scancode;
        default:
            for (i = 0; i < count; i++) {
                if (items[i].scancode == scancode) return scancode;
            }
            continue;
        }

        StartSound(SND_TEXT_TYPEWRITER);
        DrawMenuRow(x, y, width - 2, &items[*cursor], false);
        *cursor = next;
        DrawMenuRow(x, top + 3 + next, width - 2, &items[next], true);
    }
}

void PortDrawMainMenu(void)
{
    mainMenuX = DrawMenu(
        MAIN_MENU_TOP, MAIN_MENU_WIDTH, "MAIN MENU", "", mainMenuItems, MAIN_MENU_COUNT,
        mainMenuCursor
    );
}

/*
Wait for a choice from the main menu drawn by PortDrawMainMenu(). B goes back
to the title screen. Choices that leave the menu put the selection back on "New
Game" for next time.
*/
byte PortMainMenuSelect(void)
{
    byte scancode = SelectFromMenu(
        mainMenuX, MAIN_MENU_TOP, MAIN_MENU_WIDTH, mainMenuItems, MAIN_MENU_COUNT,
        &mainMenuCursor, SCANCODE_T
    );

    if (scancode == SCANCODE_B || scancode == SCANCODE_D || scancode == SCANCODE_T) {
        mainMenuCursor = 0;
    }

    return scancode;
}

void PortDrawHelpMenu(void)
{
    helpMenuCursor = 0;
    helpMenuX = DrawMenu(
        HELP_MENU_TOP, HELP_MENU_WIDTH, "HELP MENU", "Press B to go back.", helpMenuItems,
        HELP_MENU_COUNT, helpMenuCursor
    );
}

/*
Wait for a choice from the help menu drawn by PortDrawHelpMenu(). B returns
Esc, which goes back to the game.
*/
byte PortHelpMenuSelect(void)
{
    return SelectFromMenu(
        helpMenuX, HELP_MENU_TOP, HELP_MENU_WIDTH, helpMenuItems, HELP_MENU_COUNT,
        &helpMenuCursor, SCANCODE_ESC
    );
}

/*
Answer a "game number (1-9)" prompt at x,y: up and down change the number, A or
Start accepts it, and B cancels. Returns the scancode of the chosen digit, or
Esc. Starts at the number last chosen, so restoring offers the slot that was
just saved to.
*/
byte PortSelectGameNumber(word x, word y)
{
    int number = lastGameNumber;
    byte scancode;

    for (;;) {
        EGA_MODE_DEFAULT();
        DrawSpriteTile(fontTileData + FONT_UP_ARROW, x, y - 1);
        DrawSpriteTile(fontTileData + FONT_DOWN_ARROW, x, y + 1);
        DrawScancodeCharacter(x, y, SCANCODE_1 + (number - 1));

        scancode = WaitSpinner(x + 1, y);

        if (scancode == SCANCODE_KP_8 || scancode == SCANCODE_KP_6) {
            number = (number % 9) + 1;
        } else if (scancode == SCANCODE_KP_2 || scancode == SCANCODE_KP_4) {
            number = ((number + 7) % 9) + 1;
        } else if (
            scancode == SCANCODE_CTRL || scancode == SCANCODE_ENTER ||
            scancode == SCANCODE_SPACE
        ) {
            scancode = SCANCODE_1 + (number - 1);
            break;
        } else if (scancode == SCANCODE_ALT || scancode == SCANCODE_ESC) {
            scancode = SCANCODE_ESC;
            break;
        } else if (scancode >= SCANCODE_1 && scancode < SCANCODE_0) {
            number = (scancode - SCANCODE_1) + 1;
            break;
        } else {
            continue;
        }

        StartSound(SND_TEXT_TYPEWRITER);
    }

    EGA_MODE_LATCHED_WRITE();
    DRAW_SOLID_TILE_XY(TILE_DARK_GRAY, x, y - 1);
    DRAW_SOLID_TILE_XY(TILE_DARK_GRAY, x, y + 1);

    if (scancode != SCANCODE_ESC) lastGameNumber = number;

    return scancode;
}
