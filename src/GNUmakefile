#**
#* Cosmore
#* Copyright (c) 2020-2024 Scott Smitelli and contributors
#*
#* This source code is licensed under the MIT license found in the LICENSE file
#* in the root directory of this source tree.
#*

# *****************************************************************************
# *                        COSMORE SDL PORT MAKEFILE                          *
# *                                                                           *
# * Builds a native executable using SDL2 for graphics, sound, and input.     *
# * GNU make reads this file in preference to `Makefile`, which remains the   *
# * Borland MAKE.EXE build for DOS.                                           *
# *                                                                           *
# *   make                 build cosmore1                                     *
# *   make EPISODE=n       build cosmore{n} (n = 1, 2, or 3)                  *
# *   make gba             build cosmore1.gba with devkitARM (see gba/gba.mk) *
# *   make clean           remove all build products                         *
# *****************************************************************************

EPISODE ?= 1

CC ?= cc
CFLAGS ?= -O2 -g

SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS := $(shell pkg-config --libs sdl2)

# The game code was written for a compiler with 16-bit ints and a forgiving
# memory model, so keep the optimizer from exploiting undefined behavior.
PORT_CFLAGS := -std=gnu99 -fwrapv -fno-strict-aliasing -fno-delete-null-pointer-checks

# The original code trips plenty of modern warnings by design; silence those.
PORT_CFLAGS += -Wall -Wno-unused-variable -Wno-unused-but-set-variable
PORT_CFLAGS += -Wno-unused-label -Wno-unused-function -Wno-unused-result
PORT_CFLAGS += -Wno-parentheses -Wno-dangling-else -Wno-maybe-uninitialized
PORT_CFLAGS += -Wno-char-subscripts -Wno-missing-braces -Wno-unknown-pragmas
PORT_CFLAGS += -Wno-unused-value -Wno-return-type

PORT_CPPFLAGS := -DEPISODE=$(EPISODE) $(SDL_CFLAGS)
PORT_LDLIBS := $(SDL_LIBS) -lm

OBJDIR := obj/ep$(EPISODE)

# Scripted input and screenshots for automated testing; see sdl/testhooks.c.
ifdef PORT_TEST_HOOKS
PORT_CPPFLAGS += -DPORT_TEST_HOOKS
OBJDIR := obj/ep$(EPISODE)-test
endif

OUTEXE := cosmore$(EPISODE)

GAME_OBJS := $(OBJDIR)/main.o $(OBJDIR)/game1.o $(OBJDIR)/game2.o
PORT_OBJS := $(OBJDIR)/platform.o $(OBJDIR)/ega.o $(OBJDIR)/opl2.o $(OBJDIR)/testhooks.o
HEADERS := $(wildcard *.h) $(wildcard sdl/*.h)

.PHONY: all clean gba

all: $(OUTEXE)

gba:
	$(MAKE) -f gba/gba.mk EPISODE=$(EPISODE)

$(OUTEXE): $(GAME_OBJS) $(PORT_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(PORT_LDLIBS) $(LDLIBS)

$(OBJDIR)/%.o: %.c $(HEADERS) | $(OBJDIR)
	$(CC) $(PORT_CPPFLAGS) $(CPPFLAGS) $(PORT_CFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: sdl/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(PORT_CPPFLAGS) $(CPPFLAGS) $(PORT_CFLAGS) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $@

clean:
	rm -rf obj cosmore1 cosmore2 cosmore3 cosmore1.gba cosmore2.gba cosmore3.gba
