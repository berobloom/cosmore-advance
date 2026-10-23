/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                        COSMORE SDL PORT EGA EMULATION                     *
 *****************************************************************************/

#ifndef SDL_EGA_H
#define SDL_EGA_H

#include <stdint.h>

void EGAPortWrite(int port, uint8_t value);
uint8_t EGAPortRead(int port);
uint8_t EGACurrentVideoMode(void);

/*
Convert the displayed page into 320x200 32-bit XRGB pixels. Returns nonzero if
anything visible may have changed since the previous call.
*/
int EGARenderFrame(uint32_t *pixels);

#endif  /* SDL_EGA_H */
