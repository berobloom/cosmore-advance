/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                        COSMORE SDL PORT TEST HOOKS                        *
 *                                                                           *
 * Only compiled in when PORT_TEST_HOOKS is defined. Replays a script of     *
 * timed key presses and screenshots so the port can be exercised without   *
 * anyone at the keyboard. The script comes from $PORT_TEST_SCRIPT, one      *
 * command per line:                                                         *
 *                                                                           *
 *   <seconds> down <SDL key name>                                           *
 *   <seconds> up <SDL key name>                                             *
 *   <seconds> tap <SDL key name>      (press, release 0.1 seconds later)    *
 *   <seconds> shot <file.bmp>                                               *
 *   <seconds> quit                                                          *
 *                                                                           *
 * If $PORT_TEST_AUDIO is set, all generated audio is appended to that file  *
 * as raw signed 16-bit mono samples.                                        *
 *****************************************************************************/

#ifdef PORT_TEST_HOOKS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "testhooks.h"

#define MAX_EVENTS 1024

typedef struct {
    double when;
    char cmd[8];
    char arg[256];
} ScriptEvent;

static ScriptEvent events[MAX_EVENTS];
static int numEvents, nextEvent;
static double startTime = -1.0;
static FILE *audioFp;

static void LoadScript(void)
{
    const char *path = getenv("PORT_TEST_SCRIPT");
    char line[512];
    FILE *fp;

    if (path == NULL || (fp = fopen(path, "r")) == NULL) return;

    while (numEvents < MAX_EVENTS - 1 && fgets(line, sizeof line, fp) != NULL) {
        ScriptEvent *ev = &events[numEvents];

        ev->arg[0] = '\0';
        if (sscanf(line, "%lf %7s %255[^\n]", &ev->when, ev->cmd, ev->arg) < 2) continue;

        if (strcmp(ev->cmd, "tap") == 0) {
            strcpy(ev->cmd, "down");
            events[numEvents + 1] = *ev;
            events[numEvents + 1].when += 0.1;
            strcpy(events[numEvents + 1].cmd, "up");
            numEvents++;
        }
        numEvents++;
    }

    fclose(fp);
}

static int CompareEvents(const void *a, const void *b)
{
    double wa = ((const ScriptEvent *)a)->when, wb = ((const ScriptEvent *)b)->when;

    return (wa > wb) - (wa < wb);
}

void TestHookPoll(double now, void (*key)(SDL_Scancode, int), void (*shot)(const char *))
{
    if (startTime < 0.0) {
        const char *audio = getenv("PORT_TEST_AUDIO");

        startTime = now;
        LoadScript();
        qsort(events, numEvents, sizeof events[0], CompareEvents);
        if (audio != NULL) audioFp = fopen(audio, "wb");
    }

    while (nextEvent < numEvents && now - startTime >= events[nextEvent].when) {
        ScriptEvent *ev = &events[nextEvent++];

        if (strcmp(ev->cmd, "down") == 0 || strcmp(ev->cmd, "up") == 0) {
            SDL_Scancode sc = SDL_GetScancodeFromName(ev->arg);

            if (sc == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "test hook: unknown key '%s'\n", ev->arg);
            } else {
                key(sc, ev->cmd[0] == 'd');
            }
        } else if (strcmp(ev->cmd, "shot") == 0) {
            shot(ev->arg);
        } else if (strcmp(ev->cmd, "quit") == 0) {
            exit(EXIT_SUCCESS);
        }
    }
}

void TestHookAudio(const int16_t *samples, int count)
{
    if (audioFp != NULL) {
        fwrite(samples, sizeof samples[0], count, audioFp);
    }
}

#endif  /* PORT_TEST_HOOKS */
