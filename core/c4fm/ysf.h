// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Yaesu System Fusion (C4FM) layer 2: one 100 ms frame of 960 bits (120 bytes) at 9600 bit/s.
 *
 * This file belongs to the C4FM proof of concept fork. Its layout facts (interleave tables, whitening sequence, field
 * positions) were taken from the GPL-2.0-or-later MMDVMHost / MMDVM_CM sources, so the whole core/c4fm directory is
 * GPL-2.0-or-later and cannot go upstream into the MIT tree (docs/spec/c4fm.md, "License").
 *
 *   bytes   0..4    sync D4 71 C9 63 4D (40 bits, 20 symbols)
 *   bytes   5..29   FICH: 48 bits (+16 CRC) -> 4 x Golay(24,12) -> 96 bits + 4 tail -> rate 1/2 K=5 -> 200 bits,
 *                   dibit interleaved 20 x 5
 *   bytes  30..119  payload: five 144-bit blocks. V/D mode 2 (DN): each block is DCH 40 bits + VCH 104 bits; the
 *                   five DCH pieces form one 200-bit DCH (80 bits + 16 CRC + 4 tail, rate 1/2, interleaved 20 x 5).
 *                   Header / terminator: each block is 72 bits of CSD1 + 72 bits of CSD2, each 360-bit channel
 *                   carrying 160 bits + 16 CRC + 4 tail at rate 1/2, interleaved 20 x 9.
 *
 * VCH (V/D mode 2): one AMBE+2 3600x2450 frame of 49 parameter bits, the same 49 bits DMR carries (the DMR stack's
 * parameter order: bits 0-11 Golay(24,12) data, 12-23 Golay(23,12) data, 24-48 unprotected). Bits 0-26 are sent three
 * times each (81 bits), bits 27-48 once (22 bits), then one zero bit: 104 bits, whitened, bit interleaved 26 x 4.
 *
 * Bits are numbered from the most significant bit of byte 0; each symbol is two bits, the first in the higher position.
 * The dibit to deviation mapping is the DMR one (01 +3, 00 +1, 10 -1, 11 -3) at 1.39 times DMR's deviation
 * (+-2700 / +-900 Hz against +-1944 / +-648 Hz).
 */
#ifndef RFOUNDRY_YSF_H
#define RFOUNDRY_YSF_H

#include <stdbool.h>
#include <stdint.h>

#define YSF_FRAME_BYTES		120
#define YSF_FRAME_BITS		960
#define YSF_FRAME_SYMBOLS	480
#define YSF_SYNC_BYTES		5
#define YSF_FICH_BYTES		25
#define YSF_PAYLOAD_OFFSET	(YSF_SYNC_BYTES + YSF_FICH_BYTES)	// 30
#define YSF_CALLSIGN_LEN	10
#define YSF_VCH_PER_FRAME	5
#define YSF_AMBE_BITS		49
#define YSF_FRAME_MS		100

extern const uint8_t ysf_sync[YSF_SYNC_BYTES];

// FICH field values
enum { YSF_FI_HEADER = 0, YSF_FI_COMMS = 1, YSF_FI_TERMINATOR = 2, YSF_FI_TEST = 3 };
enum { YSF_DT_VD1 = 0, YSF_DT_DATA_FR = 1, YSF_DT_VD2 = 2, YSF_DT_VOICE_FR = 3 };
enum { YSF_CM_GROUP1 = 0, YSF_CM_GROUP2 = 1, YSF_CM_INDIVIDUAL = 3 };
enum { YSF_MR_DIRECT = 0, YSF_MR_NOT_BUSY = 1, YSF_MR_BUSY = 2 };

typedef struct {
	uint8_t fi;	// frame information (YSF_FI_*)
	uint8_t cs;	// callsign information: 2 on every radio heard so far
	uint8_t cm;	// call mode (YSF_CM_*)
	uint8_t bn;	// block number
	uint8_t bt;	// block total
	uint8_t fn;	// frame number in the DCH cycle (0..ft)
	uint8_t ft;	// frame total: the DCH cycle is ft + 1 frames
	bool dev;	// "deviation" bit: wide (0) / narrow (1)
	uint8_t mr;	// message route (YSF_MR_*)
	bool voip;
	uint8_t dt;	// data type (YSF_DT_*)
	bool sql;	// squelch code on
	uint8_t dgid;	// digital group ID / squelch code, 0..127
} ysf_fich_t;

typedef struct {
	uint8_t errors;		// FEC: bit errors the decoder corrected (Viterbi path metric, capped)
	bool golay_ok;		// all four Golay words decodable
	bool crc_ok;
} ysf_fec_info_t;

// --- primitives (exposed for the tests) ---

// YSF's CRC-16: CCITT polynomial 0x1021, init 0, inverted, stored big-endian in the last two bytes of buf (len total)
void ysfCrcAdd(uint8_t *buf, unsigned len);
bool ysfCrcCheck(const uint8_t *buf, unsigned len);
// Rate 1/2, K=5 convolutional code (G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4): nbits input bits -> 2 * nbits out
void ysfConvEncode(const uint8_t *in, uint8_t *out, unsigned nbits);
// Hard decision Viterbi over nsym symbol pairs (in: 2 * nsym bits), nout bits decoded (the rest are tail bits).
// Returns the path metric of the winning state (number of channel bit errors on the decoded path)
unsigned ysfConvDecode(const uint8_t *in, unsigned nsym, uint8_t *out, unsigned nout);

static inline int ysfGetBit(const uint8_t *p, unsigned i)
{
	return (p[i >> 3] >> (7u - (i & 7u))) & 1;
}

static inline void ysfPutBit(uint8_t *p, unsigned i, int v)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	if (v) {
		p[i >> 3] |= m;
	} else {
		p[i >> 3] &= (uint8_t)~m;
	}
}

// --- frame parts. frame always points at the start of a 120-byte frame ---

void ysfAddSync(uint8_t *frame);
// Count of differing bits between the frame's first 40 bits and the sync
unsigned ysfSyncErrors(const uint8_t *frame);

void ysfFichEncode(const ysf_fich_t *f, uint8_t *frame);
bool ysfFichDecode(const uint8_t *frame, ysf_fich_t *f, ysf_fec_info_t *info);

// Header and terminator payload: CSD1 = destination (10) + source (10) callsign, CSD2 = downlink (10) + uplink (10).
// Callsigns are space padded ASCII
void ysfCsdEncode(const uint8_t csd1[20], const uint8_t csd2[20], uint8_t *frame);
// Each channel decoded separately: csd1 / csd2 may be NULL; the return value has bit 0 set for a good CSD1, bit 1 CSD2
unsigned ysfCsdDecode(const uint8_t *frame, uint8_t csd1[20], uint8_t csd2[20]);

// V/D mode 2 communications payload: 10 bytes of DCH (callsign or data, by FICH fn) and five AMBE frames
// (49 bits each, one per byte as hal_vocoder_t gives them)
void ysfVd2Encode(const uint8_t dch[YSF_CALLSIGN_LEN], const uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		  uint8_t *frame);
// dch may be NULL. Returns true when the DCH's CRC was good. vch_errors (may be NULL): per VCH, the number of
// triplets the majority vote corrected (0..27)
bool ysfVd2Decode(const uint8_t *frame, uint8_t dch[YSF_CALLSIGN_LEN], uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		  uint8_t vch_errors[YSF_VCH_PER_FRAME]);

// The same decoders on soft decisions (c4fm_frame_info_t.soft: one int8 per frame bit, > 0 a 1, the magnitude its
// confidence): a soft decision Viterbi for the FICH, CSD and DCH, a weighted majority for the VCH's repeated bits.
// errors / vch_errors count the hard bits corrected, as the hard decoders do
bool ysfFichDecodeSoft(const int8_t *soft, ysf_fich_t *f, ysf_fec_info_t *info);
unsigned ysfCsdDecodeSoft(const int8_t *soft, uint8_t csd1[20], uint8_t csd2[20]);
bool ysfVd2DecodeSoft(const int8_t *soft, uint8_t dch[YSF_CALLSIGN_LEN], uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		      uint8_t vch_errors[YSF_VCH_PER_FRAME]);

// One VCH alone (exposed for the tests): 49 bits -> 13 bytes whitened and interleaved, and back
void ysfVchEncode(const uint8_t ambe[YSF_AMBE_BITS], uint8_t vch[13]);
unsigned ysfVchDecode(const uint8_t vch[13], uint8_t ambe[YSF_AMBE_BITS]);

// 49 bits of silence in the DMR parameter order (YSF's own silence pattern, for padding a short last frame)
extern const uint8_t ysf_ambe_silence[YSF_AMBE_BITS];

#endif
