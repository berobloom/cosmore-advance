#**
#* Cosmore
#* Copyright (c) 2020-2024 Scott Smitelli and contributors
#*
#* This source code is licensed under the MIT license found in the LICENSE file
#* in the root directory of this source tree.
#*

# *****************************************************************************
# *                      COSMORE GBA PORT MAKEFILE                            *
# *                                                                           *
# * Builds a Game Boy Advance ROM with devkitARM. Run from the src directory, *
# * normally through `make gba` (see GNUmakefile). The episode's COSMOx.STN   *
# * and COSMOx.VOL must be in the src directory; they go into the ROM.        *
# *****************************************************************************

EPISODE ?= 1

DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM

PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
GBA_CC := $(PREFIX)gcc
GBA_OBJCOPY := $(PREFIX)objcopy
GBAFIX := $(DEVKITPRO)/tools/bin/gbafix
HOSTCC ?= cc

STN := COSMO$(EPISODE).STN
VOL := COSMO$(EPISODE).VOL

OBJDIR := obj/gba-ep$(EPISODE)
OUTROM := cosmore$(EPISODE).gba

ARCH := -mthumb-interwork -mcpu=arm7tdmi -mtune=arm7tdmi

GBA_CPPFLAGS := -DEPISODE=$(EPISODE) -DPORT_GBA -I$(DEVKITPRO)/libgba/include
GBA_CFLAGS := -O2 -std=gnu99 -fwrapv -fno-strict-aliasing -fno-delete-null-pointer-checks

# Same warning policy as the SDL build
GBA_CFLAGS += -Wall -Wno-unused-variable -Wno-unused-but-set-variable
GBA_CFLAGS += -Wno-unused-label -Wno-unused-function -Wno-unused-result
GBA_CFLAGS += -Wno-parentheses -Wno-dangling-else -Wno-maybe-uninitialized
GBA_CFLAGS += -Wno-char-subscripts -Wno-missing-braces -Wno-unknown-pragmas
GBA_CFLAGS += -Wno-unused-value -Wno-return-type

GBA_LDFLAGS := -specs=gba.specs $(ARCH) -L$(DEVKITPRO)/libgba/lib
GBA_LIBS := -lgba

GAME_OBJS := $(OBJDIR)/main.o $(OBJDIR)/game1.o $(OBJDIR)/game2.o
PORT_OBJS := $(OBJDIR)/platform.o $(OBJDIR)/files.o $(OBJDIR)/audio.o $(OBJDIR)/video.o \
	$(OBJDIR)/menu.o
HEADERS := $(wildcard *.h) $(wildcard sdl/compat.h) $(wildcard gba/*.h)

.PHONY: all

all: $(OUTROM)

$(OUTROM): $(OBJDIR)/cosmore.elf
	$(GBA_OBJCOPY) -O binary $< $@
	$(GBAFIX) $@ -tCOSMORE$(EPISODE) -cCSM$(EPISODE) -m01

$(OBJDIR)/cosmore.elf: $(GAME_OBJS) $(PORT_OBJS) $(OBJDIR)/assets.o
	$(GBA_CC) $(GBA_LDFLAGS) -o $@ $^ $(GBA_LIBS)

# The game's globals are too big for the 32K internal RAM; move them out to
# the 256K external RAM, which the startup code also zero-fills.
$(OBJDIR)/%.o: %.c $(HEADERS) | $(OBJDIR)
	$(GBA_CC) -mthumb $(ARCH) $(GBA_CPPFLAGS) $(GBA_CFLAGS) -c -o $@ $<
	$(GBA_OBJCOPY) --rename-section .bss=.sbss $@

# Drawing and presentation run as ARM code from internal RAM.
$(OBJDIR)/video.o: gba/video.c $(HEADERS) | $(OBJDIR)
	$(GBA_CC) -marm $(ARCH) $(GBA_CPPFLAGS) $(GBA_CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: gba/%.c $(HEADERS) | $(OBJDIR)
	$(GBA_CC) -mthumb $(ARCH) $(GBA_CPPFLAGS) $(GBA_CFLAGS) -c -o $@ $<

$(OBJDIR)/assets.o: gba/assets.S $(STN) $(VOL) $(OBJDIR)/music.bin | $(OBJDIR)
	$(GBA_CC) $(ARCH) -DSTN_FILE='"$(STN)"' -DVOL_FILE='"$(VOL)"' \
		-DMUSIC_FILE='"$(OBJDIR)/music.bin"' -c -o $@ $<

$(OBJDIR)/music.bin: $(OBJDIR)/musicgen $(STN) $(VOL)
	$(OBJDIR)/musicgen $(STN) $(VOL) $@

$(OBJDIR)/musicgen: gba/musicgen.c sdl/opl2.c sdl/opl2.h | $(OBJDIR)
	$(HOSTCC) -O2 -o $@ gba/musicgen.c sdl/opl2.c -lm

$(OBJDIR):
	mkdir -p $@
