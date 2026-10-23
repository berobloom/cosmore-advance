/**
 * Cosmore
 * Copyright (c) 2020-2024 Scott Smitelli and contributors
 *
 * This source code is licensed under the MIT license found in the LICENSE file
 * in the root directory of this source tree.
 */

/*****************************************************************************
 *                    COSMORE SDL PORT YAMAHA OPL2 EMULATION                 *
 *                                                                           *
 * A sample-accurate-ish model of the YM3812 as found on the AdLib card. It  *
 * follows the publicly documented behavior of the chip (log-sin/exp lookup  *
 * tables, the rate-based envelope generator, LFOs, and rhythm mode) closely *
 * enough that the game's music sounds the way it did on real hardware.     *
 *                                                                           *
 * The two hardware timers are only modeled to the extent needed for the    *
 * game's AdLib detection routine to succeed.                                *
 *****************************************************************************/

#include <math.h>
#include <string.h>

#include "opl2.h"

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

#define KEY_NORMAL 0x01
#define KEY_DRUM   0x02

typedef struct Channel Channel;

typedef struct {
    Channel *channel;
    int num;

    int16_t out, fbmod, prout;
    const int16_t *mod;
    uint16_t egRout, egOut;
    uint8_t egGen, egKsl;
    const uint8_t *trem;

    uint8_t regVib, regType, regKsr, regMult, regKsl, regTl;
    uint8_t regAr, regDr, regSl, regRr, regWf;
    uint8_t key;
    uint8_t pgReset;
    uint32_t pgPhase;
    uint16_t pgPhaseOut;
} Slot;

struct Channel {
    Slot *slots[2];
    const int16_t *out[4];
    int num;
    uint16_t fnum;
    uint8_t block, fb, con, ksv;
    uint8_t keyOn;
};

static struct {
    Slot slots[18];
    Channel channels[9];

    uint8_t address;
    uint8_t regBD, regNts, regWse;
    uint8_t regTimer1, regTimer2, regTimerCtl, status;

    int16_t zeromod;
    uint8_t zerotrem;

    uint32_t timer;
    uint64_t egTimer;
    uint8_t egTimerRem, egState, egAdd, egTimerLo;

    uint8_t tremolo, tremoloPos, tremoloShift;
    uint8_t vibPos, vibShift;

    uint32_t noise;
    uint8_t rmHhBit2, rmHhBit3, rmHhBit7, rmHhBit8, rmTcBit3, rmTcBit5;
} chip;

static uint16_t logSinRom[256];
static uint16_t expRom[256];
static int tablesReady = 0;

static const uint8_t kslRom[16] = {
    0, 32, 40, 45, 48, 51, 53, 55, 56, 58, 59, 60, 61, 62, 63, 64
};
static const uint8_t kslShift[4] = {8, 1, 2, 0};
static const uint8_t multTable[16] = {
    1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
};
static const uint8_t egIncStep[4][4] = {
    {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 0}
};

/* Register offset (0x00..0x15) to slot number; -1 where no slot exists */
static const int8_t regToSlot[0x16] = {
    0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1,
    12, 13, 14, 15, 16, 17
};

/* First slot of each channel; the second slot is always 3 higher */
static const uint8_t channelSlot[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};

static void BuildTables(void)
{
    int i;

    for (i = 0; i < 256; i++) {
        double s = sin((i + 0.5) * M_PI / 512.0);

        logSinRom[i] = (uint16_t)floor(-log2(s) * 256.0 + 0.5);
        expRom[i] = (uint16_t)floor(pow(2.0, (255 - i) / 256.0) * 1024.0 + 0.5);
    }

    tablesReady = 1;
}

static int16_t CalcExp(uint32_t level)
{
    if (level > 0x1fff) level = 0x1fff;

    return (int16_t)(((expRom[level & 0xff] << 1) >> (level >> 8)));
}

static int16_t CalcWave(uint8_t wf, uint16_t phase, uint16_t envelope)
{
    uint16_t out;
    uint16_t neg = 0;

    phase &= 0x3ff;

    switch (wf) {
    case 0:  /* sine */
        if (phase & 0x200) neg = 0xffff;
        out = logSinRom[(phase & 0x100) ? ((phase & 0xff) ^ 0xff) : (phase & 0xff)];
        break;
    case 1:  /* half sine */
        if (phase & 0x200) {
            out = 0x1000;
        } else {
            out = logSinRom[(phase & 0x100) ? ((phase & 0xff) ^ 0xff) : (phase & 0xff)];
        }
        break;
    case 2:  /* absolute sine */
        out = logSinRom[(phase & 0x100) ? ((phase & 0xff) ^ 0xff) : (phase & 0xff)];
        break;
    default:  /* quarter sine pulses */
        if (phase & 0x100) {
            out = 0x1000;
        } else {
            out = logSinRom[phase & 0xff];
        }
        break;
    }

    return (int16_t)(CalcExp(out + (envelope << 3)) ^ neg);
}

static void UpdateKsl(Slot *slot)
{
    int ksl = (kslRom[slot->channel->fnum >> 6] << 2) - ((0x08 - slot->channel->block) << 5);

    if (ksl < 0) ksl = 0;
    slot->egKsl = (uint8_t)ksl;
}

static void EnvelopeCalc(Slot *slot)
{
    uint8_t regRate = 0;
    uint8_t ks, rate, rateHi, rateLo, shift = 0, egShift;
    uint8_t nonzero, reset = 0, egOff = 0;
    uint16_t egRout, egInc = 0;
    uint32_t out;

    out = slot->egRout + (slot->regTl << 2) + (slot->egKsl >> kslShift[slot->regKsl]) + *slot->trem;
    slot->egOut = (uint16_t)(out > 0x1ff ? 0x1ff : out);

    if (slot->key && slot->egGen == EG_RELEASE) {
        reset = 1;
        regRate = slot->regAr;
    } else {
        switch (slot->egGen) {
        case EG_ATTACK: regRate = slot->regAr; break;
        case EG_DECAY: regRate = slot->regDr; break;
        case EG_SUSTAIN: if (!slot->regType) regRate = slot->regRr; break;
        case EG_RELEASE: regRate = slot->regRr; break;
        }
    }

    slot->pgReset = reset;
    ks = slot->channel->ksv >> ((slot->regKsr ^ 1) << 1);
    nonzero = (regRate != 0);
    rate = ks + (regRate << 2);
    rateHi = rate >> 2;
    rateLo = rate & 0x03;
    if (rateHi & 0x10) rateHi = 0x0f;
    egShift = rateHi + chip.egAdd;

    if (nonzero) {
        if (rateHi < 12) {
            if (chip.egState) {
                switch (egShift) {
                case 12: shift = 1; break;
                case 13: shift = (rateLo >> 1) & 0x01; break;
                case 14: shift = rateLo & 0x01; break;
                }
            }
        } else {
            shift = (rateHi & 0x03) + egIncStep[rateLo][chip.egTimerLo];
            if (shift & 0x04) shift = 0x03;
            if (!shift) shift = chip.egState;
        }
    }

    egRout = slot->egRout;

    if (reset && rateHi == 0x0f) egRout = 0x00;  /* instant attack */
    if ((slot->egRout & 0x1f8) == 0x1f8) egOff = 1;
    if (slot->egGen != EG_ATTACK && !reset && egOff) egRout = 0x1ff;

    switch (slot->egGen) {
    case EG_ATTACK:
        if (slot->egRout == 0) {
            slot->egGen = EG_DECAY;
        } else if (slot->key && shift > 0 && rateHi != 0x0f) {
            egInc = (uint16_t)(~slot->egRout >> (4 - shift));
        }
        break;
    case EG_DECAY:
        if ((slot->egRout >> 4) == slot->regSl) {
            slot->egGen = EG_SUSTAIN;
        } else if (!egOff && !reset && shift > 0) {
            egInc = (uint16_t)(1 << (shift - 1));
        }
        break;
    case EG_SUSTAIN:
    case EG_RELEASE:
        if (!egOff && !reset && shift > 0) {
            egInc = (uint16_t)(1 << (shift - 1));
        }
        break;
    }

    slot->egRout = (egRout + egInc) & 0x1ff;

    if (reset) slot->egGen = EG_ATTACK;
    if (!slot->key) slot->egGen = EG_RELEASE;
}

static void PhaseGenerate(Slot *slot)
{
    Channel *ch = slot->channel;
    uint16_t fnum = ch->fnum;
    uint32_t basefreq;
    uint16_t phase;

    if (slot->regVib) {
        int8_t range = (fnum >> 7) & 7;
        uint8_t vibpos = chip.vibPos;

        if (!(vibpos & 3)) {
            range = 0;
        } else if (vibpos & 1) {
            range >>= 1;
        }
        range >>= chip.vibShift;
        if (vibpos & 4) range = -range;
        fnum += range;
    }

    basefreq = ((uint32_t)fnum << ch->block) >> 1;
    phase = (uint16_t)(slot->pgPhase >> 9);

    if (slot->pgReset) slot->pgPhase = 0;
    slot->pgPhase += (basefreq * multTable[slot->regMult]) >> 1;
    slot->pgPhase &= 0x7ffff;

    slot->pgPhaseOut = phase;

    if (slot->num == 13) {  /* hi-hat */
        chip.rmHhBit2 = (phase >> 2) & 1;
        chip.rmHhBit3 = (phase >> 3) & 1;
        chip.rmHhBit7 = (phase >> 7) & 1;
        chip.rmHhBit8 = (phase >> 8) & 1;
    }
    if (slot->num == 17 && (chip.regBD & 0x20)) {  /* top cymbal */
        chip.rmTcBit3 = (phase >> 3) & 1;
        chip.rmTcBit5 = (phase >> 5) & 1;
    }

    if (chip.regBD & 0x20) {
        uint8_t rmXor =
            (chip.rmHhBit2 ^ chip.rmHhBit7) |
            (chip.rmHhBit3 ^ chip.rmTcBit5) |
            (chip.rmTcBit3 ^ chip.rmTcBit5);
        uint32_t noise = chip.noise;

        switch (slot->num) {
        case 13:  /* hi-hat */
            slot->pgPhaseOut = rmXor << 9;
            if (rmXor ^ (noise & 1)) {
                slot->pgPhaseOut |= 0xd0;
            } else {
                slot->pgPhaseOut |= 0x34;
            }
            break;
        case 16:  /* snare drum */
            slot->pgPhaseOut = (chip.rmHhBit8 << 9) | ((chip.rmHhBit8 ^ (noise & 1)) << 8);
            break;
        case 17:  /* top cymbal */
            slot->pgPhaseOut = (rmXor << 9) | 0x80;
            break;
        }
    }
}

static void SlotCalcFeedback(Slot *slot)
{
    Channel *ch = slot->channel;

    if (ch->fb != 0 && slot == ch->slots[0]) {
        slot->fbmod = (int16_t)((slot->prout + slot->out) >> (0x09 - ch->fb));
    } else {
        slot->fbmod = 0;
    }
    slot->prout = slot->out;
}

static void SlotGenerate(Slot *slot)
{
    uint8_t wf = chip.regWse ? slot->regWf : 0;

    slot->out = CalcWave(wf, (uint16_t)(slot->pgPhaseOut + *slot->mod), slot->egOut);
}

/*
Recompute operator routing and output taps for every channel. Called whenever
the connection bits or rhythm mode change.
*/
static void SetupRouting(void)
{
    int i;

    for (i = 0; i < 9; i++) {
        Channel *ch = &chip.channels[i];
        int isDrum = (chip.regBD & 0x20) && i >= 6;

        ch->slots[0]->mod = &ch->slots[0]->fbmod;

        if (isDrum && (i == 7 || i == 8)) {
            ch->slots[0]->mod = &chip.zeromod;
            ch->slots[1]->mod = &chip.zeromod;
        } else if (ch->con == 0) {
            ch->slots[1]->mod = &ch->slots[0]->out;
        } else {
            ch->slots[1]->mod = &chip.zeromod;
        }

        if (isDrum) {
            if (i == 6) {
                ch->out[0] = ch->out[1] = &ch->slots[1]->out;
                ch->out[2] = ch->out[3] = &chip.zeromod;
            } else {
                ch->out[0] = ch->out[1] = &ch->slots[0]->out;
                ch->out[2] = ch->out[3] = &ch->slots[1]->out;
            }
        } else if (ch->con == 0) {
            ch->out[0] = &ch->slots[1]->out;
            ch->out[1] = ch->out[2] = ch->out[3] = &chip.zeromod;
        } else {
            ch->out[0] = &ch->slots[0]->out;
            ch->out[1] = &ch->slots[1]->out;
            ch->out[2] = ch->out[3] = &chip.zeromod;
        }
    }
}

static void UpdateChannelFrequency(Channel *ch)
{
    ch->ksv = (uint8_t)((ch->block << 1) | ((ch->fnum >> (0x09 - chip.regNts)) & 0x01));
    UpdateKsl(ch->slots[0]);
    UpdateKsl(ch->slots[1]);
}

static void WriteSlotRegister(uint8_t base, Slot *slot, uint8_t data)
{
    switch (base) {
    case 0x20:
        slot->trem = (data & 0x80) ? &chip.tremolo : &chip.zerotrem;
        slot->regVib = (data >> 6) & 1;
        slot->regType = (data >> 5) & 1;
        slot->regKsr = (data >> 4) & 1;
        slot->regMult = data & 0x0f;
        break;
    case 0x40:
        slot->regKsl = (data >> 6) & 3;
        slot->regTl = data & 0x3f;
        UpdateKsl(slot);
        break;
    case 0x60:
        slot->regAr = (data >> 4) & 0x0f;
        slot->regDr = data & 0x0f;
        break;
    case 0x80:
        slot->regSl = (data >> 4) & 0x0f;
        if (slot->regSl == 0x0f) slot->regSl = 0x1f;
        slot->regRr = data & 0x0f;
        break;
    case 0xe0:
        slot->regWf = data & 0x03;
        break;
    }
}

static void SetKey(Slot *slot, uint8_t type, int on)
{
    if (on) {
        slot->key |= type;
    } else {
        slot->key &= ~type;
    }
}

static void WriteRhythm(uint8_t data)
{
    Channel *ch6 = &chip.channels[6];
    Channel *ch7 = &chip.channels[7];
    Channel *ch8 = &chip.channels[8];
    int rhythm = (data & 0x20) != 0;

    chip.regBD = data;
    chip.tremoloShift = (uint8_t)((((data >> 7) & 1) ^ 1) << 1) + 2;
    chip.vibShift = ((data >> 6) & 1) ^ 1;

    SetupRouting();

    SetKey(ch6->slots[0], KEY_DRUM, rhythm && (data & 0x10));
    SetKey(ch6->slots[1], KEY_DRUM, rhythm && (data & 0x10));
    SetKey(ch7->slots[0], KEY_DRUM, rhythm && (data & 0x01));
    SetKey(ch7->slots[1], KEY_DRUM, rhythm && (data & 0x08));
    SetKey(ch8->slots[0], KEY_DRUM, rhythm && (data & 0x04));
    SetKey(ch8->slots[1], KEY_DRUM, rhythm && (data & 0x02));
}

void OPL2Reset(void)
{
    int i;

    if (!tablesReady) BuildTables();

    memset(&chip, 0, sizeof chip);

    for (i = 0; i < 18; i++) {
        Slot *slot = &chip.slots[i];

        slot->num = i;
        slot->mod = &chip.zeromod;
        slot->egRout = 0x1ff;
        slot->egOut = 0x1ff;
        slot->egGen = EG_RELEASE;
        slot->trem = &chip.zerotrem;
    }

    for (i = 0; i < 9; i++) {
        Channel *ch = &chip.channels[i];

        ch->num = i;
        ch->slots[0] = &chip.slots[channelSlot[i]];
        ch->slots[1] = &chip.slots[channelSlot[i] + 3];
        ch->slots[0]->channel = ch;
        ch->slots[1]->channel = ch;
    }

    chip.tremoloShift = 4;
    chip.vibShift = 1;
    chip.noise = 1;

    SetupRouting();
}

void OPL2WriteAddress(uint8_t addr)
{
    chip.address = addr;
}

void OPL2WriteData(uint8_t data)
{
    uint8_t reg = chip.address;

    if (!tablesReady) OPL2Reset();

    switch (reg & 0xf0) {
    case 0x00:
        switch (reg) {
        case 0x01:
            chip.regWse = (data >> 5) & 1;
            break;
        case 0x02:
            chip.regTimer1 = data;
            break;
        case 0x03:
            chip.regTimer2 = data;
            break;
        case 0x04:
            if (data & 0x80) {
                chip.status = 0;  /* IRQ reset */
            } else {
                chip.regTimerCtl = data;
                /*
                The timers are not clocked; a started, unmasked timer is simply
                treated as having already expired. This is all the detection
                routine needs to see.
                */
                if ((data & 0x01) && !(data & 0x40)) chip.status |= 0xc0;
                if ((data & 0x02) && !(data & 0x20)) chip.status |= 0xa0;
            }
            break;
        case 0x08:
            chip.regNts = (data >> 6) & 1;
            break;
        }
        break;

    case 0x20:
    case 0x30:
    case 0x40:
    case 0x50:
    case 0x60:
    case 0x70:
    case 0x80:
    case 0x90:
    case 0xe0:
    case 0xf0:
        {
            uint8_t off = reg & 0x1f;
            uint8_t base = (reg & 0xe0);

            if (off < 0x16 && regToSlot[off] >= 0) {
                WriteSlotRegister(base, &chip.slots[(int)regToSlot[off]], data);
            }
        }
        break;

    case 0xa0:
        if ((reg & 0x0f) < 9) {
            Channel *ch = &chip.channels[reg & 0x0f];

            ch->fnum = (ch->fnum & 0x300) | data;
            UpdateChannelFrequency(ch);
        }
        break;

    case 0xb0:
        if (reg == 0xbd) {
            WriteRhythm(data);
        } else if ((reg & 0x0f) < 9) {
            Channel *ch = &chip.channels[reg & 0x0f];

            ch->fnum = (ch->fnum & 0xff) | ((data & 0x03) << 8);
            ch->block = (data >> 2) & 0x07;
            ch->keyOn = (data >> 5) & 1;
            UpdateChannelFrequency(ch);
            SetKey(ch->slots[0], KEY_NORMAL, ch->keyOn);
            SetKey(ch->slots[1], KEY_NORMAL, ch->keyOn);
        }
        break;

    case 0xc0:
        if ((reg & 0x0f) < 9) {
            Channel *ch = &chip.channels[reg & 0x0f];

            ch->fb = (data >> 1) & 0x07;
            ch->con = data & 0x01;
            SetupRouting();
        }
        break;
    }
}

uint8_t OPL2ReadStatus(void)
{
    return chip.status | 0x06;
}

int16_t OPL2Generate(void)
{
    int i;
    int32_t mix = 0;

    if (!tablesReady) OPL2Reset();

    for (i = 0; i < 18; i++) {
        Slot *slot = &chip.slots[i];

        SlotCalcFeedback(slot);
        EnvelopeCalc(slot);
        PhaseGenerate(slot);
        SlotGenerate(slot);
    }

    for (i = 0; i < 9; i++) {
        Channel *ch = &chip.channels[i];

        mix += *ch->out[0] + *ch->out[1] + *ch->out[2] + *ch->out[3];
    }

    /* LFOs */
    if ((chip.timer & 0x3f) == 0x3f) {
        chip.tremoloPos = (chip.tremoloPos + 1) % 210;
    }
    if (chip.tremoloPos < 105) {
        chip.tremolo = chip.tremoloPos >> chip.tremoloShift;
    } else {
        chip.tremolo = (210 - chip.tremoloPos) >> chip.tremoloShift;
    }
    if ((chip.timer & 0x3ff) == 0x3ff) {
        chip.vibPos = (chip.vibPos + 1) & 7;
    }
    chip.timer++;

    /* Envelope generator clock */
    chip.egAdd = 0;
    if (chip.egTimer) {
        uint8_t shift = 0;

        while (shift < 36 && ((chip.egTimer >> shift) & 1) == 0) shift++;
        chip.egAdd = (shift > 12) ? 0 : shift + 1;
        chip.egTimerLo = (uint8_t)(chip.egTimer & 0x03);
    }
    if (chip.egTimerRem || chip.egState) {
        if (chip.egTimer == 0xfffffffffULL) {
            chip.egTimer = 0;
            chip.egTimerRem = 1;
        } else {
            chip.egTimer++;
            chip.egTimerRem = 0;
        }
    }
    chip.egState ^= 1;

    /* Rhythm noise generator */
    {
        uint32_t bit = ((chip.noise >> 14) ^ chip.noise) & 0x01;
        chip.noise = (chip.noise >> 1) | (bit << 22);
    }

    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;

    return (int16_t)mix;
}
