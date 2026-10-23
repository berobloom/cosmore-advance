# Cosmore Ports: _Cosmo_ on Linux and the Game Boy Advance

A fork of [**Cosmore**](https://github.com/smitelli/cosmore) by [Scott Smitelli](mailto:scott@smitelli.com) and [contributors](AUTHORS.md)

Cosmore is a reconstruction of the source code of _Cosmo's Cosmic Adventure_ version 1.20, written to rebuild the original DOS game exactly, quirks and bugs included. This fork takes that source and runs it in two more places:

* **Linux (and other desktops), through SDL2.** A native program that plays like the DOS original, with the PC hardware it expects (EGA graphics, timer, PC speaker, AdLib music) emulated around it.
* **The Game Boy Advance.** A cartridge ROM with the game data built in, saves in the cartridge's SRAM, a status bar and menus redesigned for the 240x160 screen, and controls built around the GBA buttons.

Both ports run the game's own code. Their changes to it are kept behind `#ifdef`s (or compile to the same thing under Turbo C), so the source still builds the original DOS game as described in [Building for DOS](#building-for-dos).

## Game Data

No game data is included. You need `COSMOx.STN` and `COSMOx.VOL` for each episode you want to play, where _x_ is the episode number. Episode 1 is shareware and its files are widely available. Episodes 2 and 3 are the registered versions, and their files are **not** free to download.

The GBA build puts these files inside the ROM, so a ROM carries the same restrictions as the episode's data files: don't share ROMs of episodes 2 and 3.

## Game Boy Advance

### Building

Requirements:

* The devkitPro GBA toolchain. Install devkitPro's pacman (see [Getting Started](https://devkitpro.org/wiki/Getting_Started)), then run `sudo dkp-pacman -S gba-dev` and `source /etc/profile.d/devkit-env.sh`.
* GNU make and a C compiler for the host, which builds the music renderer.

Put `COSMOx.STN` and `COSMOx.VOL` in the `src` directory, then from there:

    make gba               # builds cosmore1.gba
    make gba EPISODE=2     # builds cosmore2.gba (likewise for 3)
    make clean             # removes the SDL and GBA builds

The ROM is about 14 MB. It runs in emulators such as [mGBA](https://mgba.io/) and on a GBA (tested on a GBA SP) from a flash cartridge. If the cartridge's menu asks for a save type, choose SRAM.

### Controls

| Button | Does                                      | PC key it stands for |
|--------|-------------------------------------------|----------------------|
| D-pad  | Walk, look up and down; move in menus     | Arrow keys           |
| A      | Jump; choose in menus                     | Ctrl                 |
| B      | Drop a bomb; go back in menus             | Alt                  |
| Start  | Choose in menus                           | Enter                |
| L      | Quit the game                             | Esc                  |
| R      | Help menu: save, restore, hints           | F1                   |
| Select | Open the on-screen keyboard               |                      |

The on-screen keyboard reaches every other key the game uses, such as P to pause, S to toggle sound, and M to toggle music. Move with the D-pad, type with A, and close it with B or Select.

The main menu, the in-game help menu, and the "game number (1-9)" prompts for saving and restoring are all driven by the D-pad. The main menu leaves out the options that have no use on a GBA: ordering information, Apogee's BBS, key redefinition, and quitting. When asked to confirm quitting, A or Start answers yes. The instructions end with a page showing these controls.

### Differences From the DOS Game

* **A smaller view.** The GBA screen is 240x160, so the play area shows 30x15 tiles instead of 38x18. The status bar is rebuilt to fit beneath it: its four groups (score, health, bombs, stars) stay whole, and only plain rock, the corner rivets, some border lines, and the "Press F1 for help" note are left out.
* **Scaled screens.** Dialogs and menus are shown at full size when they fit. Bigger screens, like the full-screen pictures and story pages, are scaled down to fit.
* **Prerecorded music.** The GBA can't emulate the AdLib's OPL2 chip in real time, so every song is rendered to audio when the ROM is built, using the SDL port's OPL2 emulator, and stored in the ROM. Sound effects play on the GBA's own square-wave channel, much like the PC speaker they were written for.
* **Saves in SRAM.** Configuration, high scores, and saved games are stored in the cartridge's battery-backed SRAM.
* **Quitting resets the cartridge,** instead of showing the DOS exit screen. Joysticks are not supported.

## Linux (SDL2)

Requirements: GNU make, a C compiler, `pkg-config`, and the SDL2 development package (e.g. `libsdl2-dev` on Debian/Ubuntu). From the `src` directory:

    make                # builds cosmore1
    make EPISODE=2      # builds cosmore2 (likewise for 3)
    make clean

To play, put `COSMOx.STN` and `COSMOx.VOL` in the current directory (file names are matched case-insensitively) and run `./cosmorex`. The game plays with the keyboard, just like on DOS. Alt+Enter toggles fullscreen. Configuration and save files are written to the current directory, and the exit text screen is printed to the terminal. Joysticks are not supported.

Building with `make PORT_TEST_HOOKS=1` enables scripted input, screenshots, and audio capture for automated testing; see `src/sdl/testhooks.c`.

## How the Ports Work

The game expects a 1992 PC: it writes to EGA video memory, programs the timer chip, toggles the PC speaker, sends music to an AdLib card, and reads the keyboard through its own interrupt handler. The ports keep all of that code and supply the hardware underneath it.

* `src/GNUmakefile` builds the SDL port, and its `gba` target hands off to `src/gba/gba.mk`. GNU make reads it ahead of the Borland `Makefile`.
* `src/sdl/compat.h` stands in for the Turbo C headers and DOS functions, and declares the hooks the GBA port adds.
* `src/sdl/` is the SDL port: EGA planes and latches (`ega.c`), an OPL2 emulator (`opl2.c`), and the timer, speaker, keyboard, window, and files (`platform.c`).
* `src/gba/` is the GBA port:
  * `video.c` draws the EGA pages into the GBA's bitmap mode, choosing for each screen whether to show it 1:1, cropped, or scaled, and builds the fitted status bar.
  * `audio.c` streams the prerecorded music and drives the speaker channel.
  * `files.c` serves the group files from ROM and keeps saves in SRAM.
  * `platform.c` handles startup, interrupts, the buttons, and the on-screen keyboard.
  * `menu.c` holds the D-pad menus.
  * `musicgen.c` is the build-time music renderer.

## Building for DOS

The DOS build is unchanged from Cosmore. In short, it needs Borland Turbo C 2.0 and Turbo Assembler 1.0 in a DOS environment such as [DOSBox](https://www.dosbox.com/), installed under `C:\TC20` and on the `PATH`. From the `src` directory:

    make clean
    make -DEPISODE=n

where _n_ is `1`, `2`, or `3`. **Always** run `make clean` before changing the episode number. The game is built as `COSMOREx.EXE`; run it from a directory that also holds `COSMOx.STN` and `COSMOx.VOL`. `build.bat` builds a release of all three episodes into a `DIST` directory.

These ports have only been built and tested with GCC and devkitARM; the DOS build of this fork has not been checked against the originals. The full guide to setting up the toolchain, along with how closely the reconstruction matches the original game, is in [Cosmore's README](https://github.com/smitelli/cosmore#readme).

## Learning More

Cosmore's code is commented at the function level. The companion project [**Cosmodoc**](https://cosmodoc.org/) documents it in depth, including the [global variables and constants](https://cosmodoc.org/topics/global-variables-and-constants/), an [index of all functions](https://cosmodoc.org/topics/function-index/), and a [list of topics](https://cosmodoc.org/topics/) covering the game's behavior and the PC hardware it uses.

The port code follows Cosmore's naming and style conventions, which are described in [Cosmore's README](https://github.com/smitelli/cosmore#code-conventions).

## Credits

* The reconstructed game source is the work of [Cosmore](https://github.com/smitelli/cosmore)'s author and [contributors](AUTHORS.md).
* `src/C0.ASM` is the Turbo C 2.0 startup code, copyright 1988 by Borland International, with a one-line change to include `lowlevel.asm`.
* `src/LZEXE91/LZEXE.EXE` is LZEXE 0.91, copyright 1989 by Fabrice Bellard, used by `build.bat` to compress DOS releases like the original game.
* The GBA port is built with [devkitPro](https://devkitpro.org/)'s devkitARM and links against libgba.

## License

Except for `src/C0.ASM` and the files in `src/LZEXE91/`, MIT. All rights and trademarks to this game and its associated assets belong to Apogee Software.
