/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE GBA PORT ASSETS                          *
 *                                                                           *
 * Builds the episode's group files and prerendered music into the ROM. The  *
 * makefile passes in the file names.                                        *
 *****************************************************************************/

    .section .rodata

    /* Tells emulators and flash carts that the cartridge has 32K SRAM. */
    .balign 4
    .ascii "SRAM_V113"
    .balign 4

    .global romStnStart, romStnEnd, romVolStart, romVolEnd, musicBlob

    .balign 4
romStnStart:
    .incbin STN_FILE
romStnEnd:

    .balign 4
romVolStart:
    .incbin VOL_FILE
romVolEnd:

    .balign 4
musicBlob:
    .incbin MUSIC_FILE
