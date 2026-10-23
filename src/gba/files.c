/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                          COSMORE GBA PORT FILES                           *
 *                                                                           *
 * A stdio device (newlib devoptab) with two kinds of files:                 *
 *                                                                           *
 * - The episode's STN and VOL group files, built into the cartridge ROM.    *
 *   These are read-only, and the game can use their contents in place; see  *
 *   PortGroupEntryROM() and PortFread().                                    *
 * - Everything the game writes (configuration, high scores, saved games),   *
 *   kept in the cartridge's 32K battery-backed SRAM. The SRAM holds a small *
 *   directory followed by fixed-size slots, one per file.                   *
 *                                                                           *
 * File names are matched case-insensitively, ignoring any directory part.   *
 *****************************************************************************/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>

#include "gbaport.h"

#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)
#define FILENAME_BASE "COSMO" STRINGIFY(EPISODE)

#define SRAM            ((volatile uint8_t *)0x0e000000)
#define SRAM_MAGIC      "CSMRSRM1"
#define SRAM_SLOTS      15
#define SRAM_NAME_LEN   13
#define SRAM_DIR_START  16
#define SRAM_DIR_ENTRY  16  /* name, then 16-bit size */
#define SRAM_DATA_START 256
#define SRAM_SLOT_SIZE  ((32768 - SRAM_DATA_START) / SRAM_SLOTS)

enum {FILE_ROM, FILE_SRAM};

typedef struct {
    int kind;
    const uint8_t *rom;  /* FILE_ROM */
    int slot;  /* FILE_SRAM */
    uint8_t *buffer;  /* FILE_SRAM opened for writing */
    int writing;
    uint32_t size, pos;
} PortFile;

extern const uint8_t romStnStart[], romStnEnd[], romVolStart[], romVolEnd[];

/* The two group files embedded by assets.S */
static const char *const romNames[2] = {FILENAME_BASE ".STN", FILENAME_BASE ".VOL"};

/*****************************************************************************
 * SRAM                                                                      *
 *****************************************************************************/

static void SramRead(unsigned addr, void *dest, unsigned length)
{
    uint8_t *d = dest;

    while (length-- > 0) *d++ = SRAM[addr++];
}

static void SramWrite(unsigned addr, const void *src, unsigned length)
{
    const uint8_t *s = src;

    while (length-- > 0) SRAM[addr++] = *s++;
}

static void SramEntry(int slot, char *name, unsigned *size)
{
    unsigned addr = SRAM_DIR_START + (slot * SRAM_DIR_ENTRY);
    uint8_t sz[2];

    SramRead(addr, name, SRAM_NAME_LEN);
    name[SRAM_NAME_LEN - 1] = '\0';
    SramRead(addr + SRAM_NAME_LEN, sz, 2);
    *size = sz[0] | (sz[1] << 8);
}

static void SramSetEntry(int slot, const char *name, unsigned size)
{
    unsigned addr = SRAM_DIR_START + (slot * SRAM_DIR_ENTRY);
    char n[SRAM_NAME_LEN];
    uint8_t sz[2];

    memset(n, 0, sizeof n);
    strncpy(n, name, SRAM_NAME_LEN - 1);
    sz[0] = (uint8_t)(size & 0xff);
    sz[1] = (uint8_t)(size >> 8);
    SramWrite(addr, n, SRAM_NAME_LEN);
    SramWrite(addr + SRAM_NAME_LEN, sz, 2);
}

static int SramFind(const char *name)
{
    char entry[SRAM_NAME_LEN];
    unsigned size;
    int slot;

    for (slot = 0; slot < SRAM_SLOTS; slot++) {
        SramEntry(slot, entry, &size);
        if (entry[0] != '\0' && strcmp(entry, name) == 0) return slot;
    }

    return -1;
}

static int SramFindFree(void)
{
    char entry[SRAM_NAME_LEN];
    unsigned size;
    int slot;

    for (slot = 0; slot < SRAM_SLOTS; slot++) {
        SramEntry(slot, entry, &size);
        if (entry[0] == '\0') return slot;
    }

    return -1;
}

static void SramFormatIfNeeded(void)
{
    char magic[8];
    int slot;

    SramRead(0, magic, 8);
    if (memcmp(magic, SRAM_MAGIC, 8) == 0) return;

    for (slot = 0; slot < SRAM_SLOTS; slot++) {
        SramSetEntry(slot, "", 0);
    }
    SramWrite(0, SRAM_MAGIC, 8);
}

/*****************************************************************************
 * The device                                                                *
 *****************************************************************************/

/* Reduce a path to an upper-case bare file name */
static void BaseName(const char *path, char *out, size_t outlen)
{
    const char *p = path, *colon = strchr(path, ':');
    size_t i = 0;

    if (colon != NULL) p = colon + 1;
    for (const char *s = p; *s != '\0'; s++) {
        if (*s == '/' || *s == '\\') p = s + 1;
    }

    while (*p != '\0' && i < outlen - 1) {
        out[i++] = (char)toupper((unsigned char)*p++);
    }
    out[i] = '\0';
}

static int DevOpen(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
{
    PortFile *f = fileStruct;
    char name[64];
    int access = flags & O_ACCMODE;

    (void) mode;
    memset(f, 0, sizeof *f);
    BaseName(path, name, sizeof name);

    if (access == O_RDONLY) {
        if (strcmp(name, romNames[0]) == 0) {
            f->kind = FILE_ROM;
            f->rom = romStnStart;
            f->size = (uint32_t)(romStnEnd - romStnStart);
            return 0;
        }
        if (strcmp(name, romNames[1]) == 0) {
            f->kind = FILE_ROM;
            f->rom = romVolStart;
            f->size = (uint32_t)(romVolEnd - romVolStart);
            return 0;
        }
    }

    if (strlen(name) >= SRAM_NAME_LEN) {
        r->_errno = ENOENT;
        return -1;
    }

    f->kind = FILE_SRAM;
    f->slot = SramFind(name);

    if (access == O_RDONLY) {
        char entry[SRAM_NAME_LEN];
        unsigned size;

        if (f->slot < 0) {
            r->_errno = ENOENT;
            return -1;
        }
        SramEntry(f->slot, entry, &size);
        f->size = size;
        return 0;
    }

    /* Writing; anything already in the file is replaced when it's closed */
    if (f->slot < 0) f->slot = SramFindFree();
    if (f->slot < 0) {
        r->_errno = ENOSPC;
        return -1;
    }
    f->buffer = malloc(SRAM_SLOT_SIZE);
    if (f->buffer == NULL) {
        r->_errno = ENOMEM;
        return -1;
    }
    f->writing = 1;
    SramSetEntry(f->slot, name, 0);

    return 0;
}

static int DevClose(struct _reent *r, void *fd)
{
    PortFile *f = fd;

    (void) r;

    if (f->writing) {
        char entry[SRAM_NAME_LEN];
        unsigned size;

        SramEntry(f->slot, entry, &size);
        SramWrite(SRAM_DATA_START + (f->slot * SRAM_SLOT_SIZE), f->buffer, f->size);
        SramSetEntry(f->slot, entry, f->size);
        free(f->buffer);
    }

    return 0;
}

static ssize_t DevRead(struct _reent *r, void *fd, char *ptr, size_t len)
{
    PortFile *f = fd;

    (void) r;

    if (f->pos >= f->size) return 0;
    if (len > f->size - f->pos) len = f->size - f->pos;

    if (f->kind == FILE_ROM) {
        memcpy(ptr, f->rom + f->pos, len);
    } else if (f->writing) {
        memcpy(ptr, f->buffer + f->pos, len);
    } else {
        SramRead(SRAM_DATA_START + (f->slot * SRAM_SLOT_SIZE) + f->pos, ptr, len);
    }
    f->pos += len;

    return (ssize_t)len;
}

static ssize_t DevWrite(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    PortFile *f = fd;

    if (!f->writing) {
        r->_errno = EBADF;
        return -1;
    }
    if (f->pos + len > SRAM_SLOT_SIZE) {
        len = SRAM_SLOT_SIZE - f->pos;
        if (len == 0) {
            r->_errno = ENOSPC;
            return -1;
        }
    }

    memcpy(f->buffer + f->pos, ptr, len);
    f->pos += len;
    if (f->pos > f->size) f->size = f->pos;

    return (ssize_t)len;
}

static off_t DevSeek(struct _reent *r, void *fd, off_t pos, int dir)
{
    PortFile *f = fd;
    off_t base = dir == SEEK_CUR ? (off_t)f->pos : dir == SEEK_END ? (off_t)f->size : 0;

    if (base + pos < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    f->pos = (uint32_t)(base + pos);

    return (off_t)f->pos;
}

static int DevFstat(struct _reent *r, void *fd, struct stat *st)
{
    PortFile *f = fd;

    (void) r;
    memset(st, 0, sizeof *st);
    st->st_size = (off_t)f->size;
    st->st_mode = S_IFREG;

    return 0;
}

static int DevUnlink(struct _reent *r, const char *path)
{
    char name[64];
    int slot;

    BaseName(path, name, sizeof name);
    slot = SramFind(name);
    if (slot < 0) {
        r->_errno = ENOENT;
        return -1;
    }
    SramSetEntry(slot, "", 0);

    return 0;
}

static const devoptab_t cartDevice = {
    .name = "cart",
    .structSize = sizeof(PortFile),
    .open_r = DevOpen,
    .close_r = DevClose,
    .write_r = DevWrite,
    .read_r = DevRead,
    .seek_r = DevSeek,
    .fstat_r = DevFstat,
    .unlink_r = DevUnlink
};

void FilesInit(void)
{
    SramFormatIfNeeded();
    setDefaultDevice(AddDevice(&cartDevice));
}

/*
Where the file's current read position is in ROM, or NULL if the file isn't a
ROM file.
*/
const unsigned char *FileROMPointer(FILE *fp)
{
    __handle *handle;
    PortFile *f;

    if (fp == NULL) return NULL;

    handle = __get_handle(fileno(fp));
    if (handle == NULL) return NULL;

    f = handle->fileStruct;
    if (f == NULL || f->kind != FILE_ROM) return NULL;

    return f->rom + ftell(fp);
}

/*****************************************************************************
 * Game-facing functions                                                     *
 *****************************************************************************/

unsigned char *PortGroupEntryROM(char *entry_name)
{
    FILE *fp = GroupEntryFp(entry_name);
    unsigned char *ptr = (unsigned char *)FileROMPointer(fp);

    fclose(fp);

    return ptr;
}

/*
Read the way fread() does, except that reading into the file's own ROM data
(where the game points at ROM instead of allocating a copy) just skips ahead.
*/
size_t PortFread(void *ptr, size_t size, size_t count, FILE *fp)
{
    if (ptr != NULL && ptr == (void *)FileROMPointer(fp)) {
        fseek(fp, (long)(size * count), SEEK_CUR);
        return count;
    }

    return fread(ptr, size, count, fp);
}

/*
Files are unbuffered: ROM reads are memory copies anyway, and stdio buffers
would eat into the little RAM there is.
*/
FILE *PortFopen(const char *filename, const char *mode)
{
    FILE *fp = fopen(filename, mode);

    if (fp != NULL) setvbuf(fp, NULL, _IONBF, 0);

    return fp;
}

int PortFclose(FILE *fp)
{
    if (fp == NULL) return EOF;

    return fclose(fp);
}

int PortRemove(const char *filename)
{
    return remove(filename);
}

int PortGetw(FILE *fp)
{
    int lo = fgetc(fp);
    int hi = fgetc(fp);

    if (lo == EOF || hi == EOF) return EOF;

    return lo | (hi << 8);
}

int PortPutw(int w, FILE *fp)
{
    if (fputc(w & 0xff, fp) == EOF) return EOF;
    if (fputc((w >> 8) & 0xff, fp) == EOF) return EOF;

    return w;
}
