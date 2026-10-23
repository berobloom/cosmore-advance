/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                        COSMORE SDL PORT TEST HOOKS                        *
 *****************************************************************************/

#ifndef SDL_TESTHOOKS_H
#define SDL_TESTHOOKS_H

#ifdef PORT_TEST_HOOKS

#include <stdint.h>

#include <SDL.h>

void TestHookPoll(double now, void (*key)(SDL_Scancode, int), void (*shot)(const char *));
void TestHookAudio(const int16_t *samples, int count);

#endif  /* PORT_TEST_HOOKS */

#endif  /* SDL_TESTHOOKS_H */
