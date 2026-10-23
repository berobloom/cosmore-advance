/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                    COSMORE SDL PORT YAMAHA OPL2 EMULATION                 *
 *****************************************************************************/

#ifndef SDL_OPL2_H
#define SDL_OPL2_H

#include <stdint.h>

/* Native sample rate of the chip: 3.579545 MHz / 72 */
#define OPL2_SAMPLE_RATE 49716

void OPL2Reset(void);
void OPL2WriteAddress(uint8_t addr);
void OPL2WriteData(uint8_t data);
uint8_t OPL2ReadStatus(void);
int16_t OPL2Generate(void);

#endif  /* SDL_OPL2_H */
