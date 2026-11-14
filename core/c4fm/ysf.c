// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion layer 2 (ysf.h for the frame layout and the license note)
#include "ysf.h"

#include <stddef.h>
#include <string.h>

#include "dmr/ambe/ambeFec.h"	// the Golay(24,12) code is the one AMBE+2 uses for its C0 word

const uint8_t ysf_sync[YSF_SYNC_BYTES] = { 0xD4, 0x71, 0xC9, 0x63, 0x4D };

// Whitening sequence for DCH, CSD and VCH contents (20 bytes; a VCH uses the first 13)
static const uint8_t whitening[20] = {
	0x93, 0xD7, 0x51, 0x21, 0x9C, 0x2F, 0x6C, 0xD0, 0xEF, 0x0F,
	0xF8, 0x3D, 0xF1, 0x73, 0x20, 0x94, 0xED, 0x1E, 0x7C, 0xD8,
};

// --- CRC ---

static uint16_t crcRun(const uint8_t *buf, unsigned n)
{
	uint16_t crc = 0;
	for (unsigned i = 0; i < n; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
		}
	}
	return (uint16_t)~crc;
}

void ysfCrcAdd(uint8_t *buf, unsigned len)
{
	uint16_t crc = crcRun(buf, len - 2);
	buf[len - 2] = (uint8_t)(crc >> 8);
	buf[len - 1] = (uint8_t)crc;
}

bool ysfCrcCheck(const uint8_t *buf, unsigned len)
{
	uint16_t crc = crcRun(buf, len - 2);
	return buf[len - 2] == (uint8_t)(crc >> 8) && buf[len - 1] == (uint8_t)crc;
}

// --- convolutional code ---
// Encoder state s = d1 + 2 d2 + 4 d3 + 8 d4 (d1 the newest input bit). Input d gives G1 = d ^ d3 ^ d4,
// G2 = d ^ d1 ^ d2 ^ d4 and the next state ((s << 1) | d) & 15.

static inline uint8_t convOut(uint8_t s, uint8_t d)
{
	uint8_t d1 = s & 1u, d2 = (s >> 1) & 1u, d3 = (s >> 2) & 1u, d4 = (s >> 3) & 1u;
	uint8_t g1 = d ^ d3 ^ d4;
	uint8_t g2 = d ^ d1 ^ d2 ^ d4;
	return (uint8_t)((g1 << 1) | g2);
}

void ysfConvEncode(const uint8_t *in, uint8_t *out, unsigned nbits)
{
	uint8_t s = 0;
	for (unsigned i = 0; i < nbits; i++) {
		uint8_t d = (uint8_t)ysfGetBit(in, i);
		uint8_t g = convOut(s, d);
		ysfPutBit(out, 2 * i, g >> 1);
		ysfPutBit(out, 2 * i + 1, g & 1);
		s = (uint8_t)(((s << 1) | d) & 15u);
	}
}

#define CONV_MAX_SYM	180	// the CSD channels: 176 bits + 4 tail

unsigned ysfConvDecode(const uint8_t *in, unsigned nsym, uint8_t *out, unsigned nout)
{
	uint16_t metric[16], next[16];
	uint16_t dec[CONV_MAX_SYM];	// per step: bit ns = which predecessor won (its d4)
	if (nsym > CONV_MAX_SYM) {
		nsym = CONV_MAX_SYM;
	}
	for (int s = 0; s < 16; s++) {
		metric[s] = s == 0 ? 0 : 0x3FFF;	// the encoder starts in state 0
	}
	for (unsigned t = 0; t < nsym; t++) {
		uint8_t rx = (uint8_t)((ysfGetBit(in, 2 * t) << 1) | ysfGetBit(in, 2 * t + 1));
		uint16_t d = 0;
		for (int ns = 0; ns < 16; ns++) {
			uint8_t bit = (uint8_t)(ns & 1);
			uint8_t p0 = (uint8_t)(ns >> 1), p1 = (uint8_t)((ns >> 1) | 8);
			uint8_t e0 = convOut(p0, bit) ^ rx, e1 = convOut(p1, bit) ^ rx;
			uint16_t m0 = (uint16_t)(metric[p0] + (e0 >> 1) + (e0 & 1));
			uint16_t m1 = (uint16_t)(metric[p1] + (e1 >> 1) + (e1 & 1));
			if (m1 < m0) {
				next[ns] = m1;
				d |= (uint16_t)(1u << ns);
			} else {
				next[ns] = m0;
			}
		}
		dec[t] = d;
		memcpy(metric, next, sizeof metric);
	}
	// The tail bits bring the encoder back to state 0
	uint8_t s = 0;
	unsigned cost = metric[0];
	for (unsigned t = nsym; t-- > 0;) {
		uint8_t bit = s & 1u;
		if (t < nout) {
			ysfPutBit(out, t, bit);
		}
		s = (uint8_t)((s >> 1) | (((dec[t] >> s) & 1u) << 3));
	}
	return cost;
}

// Soft decision Viterbi: in[2 nsym] (> 0 a 1, magnitude the confidence), the same trellis; the branch cost of an
// expected bit is how far the soft value is from it (0..254)
// The soft decoders' buffers are static (off the caller's stack: the radio task's is small); they are not reentrant,
// and run in one task only
static unsigned convDecodeSoft(const int8_t *in, unsigned nsym, uint8_t *out, unsigned nout)
{
	uint32_t metric[16], next[16];
	static uint16_t dec[CONV_MAX_SYM];
	if (nsym > CONV_MAX_SYM) {
		nsym = CONV_MAX_SYM;
	}
	for (int s = 0; s < 16; s++) {
		metric[s] = s == 0 ? 0 : 0x3FFFFFu;
	}
	for (unsigned t = 0; t < nsym; t++) {
		int32_t a = in[2 * t], b = in[2 * t + 1];
		// cost of each expected pair (g1 g2): 00, 01, 10, 11
		uint32_t cost[4] = {
			(uint32_t)((127 + a) + (127 + b)), (uint32_t)((127 + a) + (127 - b)),
			(uint32_t)((127 - a) + (127 + b)), (uint32_t)((127 - a) + (127 - b)),
		};
		uint16_t d = 0;
		for (int ns = 0; ns < 16; ns++) {
			uint8_t bit = (uint8_t)(ns & 1);
			uint8_t p0 = (uint8_t)(ns >> 1), p1 = (uint8_t)((ns >> 1) | 8);
			uint32_t m0 = metric[p0] + cost[convOut(p0, bit)];
			uint32_t m1 = metric[p1] + cost[convOut(p1, bit)];
			if (m1 < m0) {
				next[ns] = m1;
				d |= (uint16_t)(1u << ns);
			} else {
				next[ns] = m0;
			}
		}
		dec[t] = d;
		memcpy(metric, next, sizeof metric);
	}
	uint8_t s = 0;
	for (unsigned t = nsym; t-- > 0;) {
		if (t < nout) {
			ysfPutBit(out, t, s & 1u);
		}
		s = (uint8_t)((s >> 1) | (((dec[t] >> s) & 1u) << 3));
	}
	// The hard errors corrected (comparable with ysfConvDecode's cost): the re-encoded path against the signs
	static uint8_t re[CONV_MAX_SYM / 4 + 1];
	ysfConvEncode(out, re, nsym < nout ? nsym : nout);
	unsigned errs = 0;
	for (unsigned t = 0; t < (nsym < nout ? nsym : nout); t++) {
		errs += (unsigned)(ysfGetBit(re, 2 * t) != (in[2 * t] > 0)) + (unsigned)(ysfGetBit(re, 2 * t + 1) != (in[2 * t + 1] > 0));
	}
	return errs;
}

// --- sync ---

void ysfAddSync(uint8_t *frame)
{
	memcpy(frame, ysf_sync, YSF_SYNC_BYTES);
}

unsigned ysfSyncErrors(const uint8_t *frame)
{
	unsigned n = 0;
	for (int i = 0; i < YSF_SYNC_BYTES; i++) {
		uint8_t x = frame[i] ^ ysf_sync[i];
		while (x) {
			x &= (uint8_t)(x - 1);
			n++;
		}
	}
	return n;
}

// --- dibit interleave (20 rows x cols columns): channel symbol i goes to dibit (i % cols) * 20 + i / cols ---

static void dibitInterleave(const uint8_t *conv, uint8_t *dst, unsigned cols)
{
	unsigned n = 20 * cols;
	for (unsigned i = 0; i < n; i++) {
		unsigned pos = 2 * ((i % cols) * 20 + i / cols);
		ysfPutBit(dst, pos, ysfGetBit(conv, 2 * i));
		ysfPutBit(dst, pos + 1, ysfGetBit(conv, 2 * i + 1));
	}
}

static void dibitDeinterleave(const uint8_t *src, uint8_t *conv, unsigned cols)
{
	unsigned n = 20 * cols;
	for (unsigned i = 0; i < n; i++) {
		unsigned pos = 2 * ((i % cols) * 20 + i / cols);
		ysfPutBit(conv, 2 * i, ysfGetBit(src, pos));
		ysfPutBit(conv, 2 * i + 1, ysfGetBit(src, pos + 1));
	}
}

// The soft values in the channel's order from frame bit positions (the inverse of dibitInterleave)
static void softDeinterleave(const int8_t *src, int8_t *conv, unsigned cols)
{
	unsigned n = 20 * cols;
	for (unsigned i = 0; i < n; i++) {
		unsigned pos = 2 * ((i % cols) * 20 + i / cols);
		conv[2 * i] = src[pos];
		conv[2 * i + 1] = src[pos + 1];
	}
}

// --- FICH ---

static void fichPack(const ysf_fich_t *f, uint8_t b[6])
{
	b[0] = (uint8_t)(((f->fi & 3u) << 6) | ((f->cs & 3u) << 4) | ((f->cm & 3u) << 2) | (f->bn & 3u));
	b[1] = (uint8_t)(((f->bt & 3u) << 6) | ((f->fn & 7u) << 3) | (f->ft & 7u));
	b[2] = (uint8_t)((f->dev ? 0x40u : 0) | ((f->mr & 7u) << 3) | (f->voip ? 0x04u : 0) | (f->dt & 3u));
	b[3] = (uint8_t)((f->sql ? 0x80u : 0) | (f->dgid & 0x7Fu));
}

static void fichUnpack(const uint8_t b[6], ysf_fich_t *f)
{
	f->fi = (uint8_t)(b[0] >> 6);
	f->cs = (uint8_t)((b[0] >> 4) & 3u);
	f->cm = (uint8_t)((b[0] >> 2) & 3u);
	f->bn = (uint8_t)(b[0] & 3u);
	f->bt = (uint8_t)(b[1] >> 6);
	f->fn = (uint8_t)((b[1] >> 3) & 7u);
	f->ft = (uint8_t)(b[1] & 7u);
	f->dev = (b[2] & 0x40u) != 0;
	f->mr = (uint8_t)((b[2] >> 3) & 7u);
	f->voip = (b[2] & 0x04u) != 0;
	f->dt = (uint8_t)(b[2] & 3u);
	f->sql = (b[3] & 0x80u) != 0;
	f->dgid = (uint8_t)(b[3] & 0x7Fu);
}

void ysfFichEncode(const ysf_fich_t *f, uint8_t *frame)
{
	uint8_t b[6];
	fichPack(f, b);
	ysfCrcAdd(b, 6);
	uint16_t d[4] = {
		(uint16_t)((b[0] << 4) | (b[1] >> 4)),
		(uint16_t)(((b[1] & 0x0Fu) << 8) | b[2]),
		(uint16_t)((b[3] << 4) | (b[4] >> 4)),
		(uint16_t)(((b[4] & 0x0Fu) << 8) | b[5]),
	};
	uint8_t words[13] = { 0 };	// 96 bits + 4 tail bits (zero)
	for (int i = 0; i < 4; i++) {
		uint32_t c = ambeGolay24Encode(d[i]);
		words[3 * i + 0] = (uint8_t)(c >> 16);
		words[3 * i + 1] = (uint8_t)(c >> 8);
		words[3 * i + 2] = (uint8_t)c;
	}
	uint8_t conv[25];
	ysfConvEncode(words, conv, 100);
	dibitInterleave(conv, frame + YSF_SYNC_BYTES, 5);
}

bool ysfFichDecode(const uint8_t *frame, ysf_fich_t *f, ysf_fec_info_t *info)
{
	uint8_t conv[25], words[13] = { 0 };
	dibitDeinterleave(frame + YSF_SYNC_BYTES, conv, 5);
	unsigned cost = ysfConvDecode(conv, 100, words, 96);
	uint16_t d[4];
	bool golay = true;
	for (int i = 0; i < 4; i++) {
		uint32_t c = ((uint32_t)words[3 * i] << 16) | ((uint32_t)words[3 * i + 1] << 8) | words[3 * i + 2];
		if (ambeGolay24Decode(c, &d[i]) < 0) {
			golay = false;
		}
	}
	uint8_t b[6] = {
		(uint8_t)(d[0] >> 4), (uint8_t)(((d[0] & 0x0Fu) << 4) | (d[1] >> 8)), (uint8_t)d[1],
		(uint8_t)(d[2] >> 4), (uint8_t)(((d[2] & 0x0Fu) << 4) | (d[3] >> 8)), (uint8_t)d[3],
	};
	bool crc = ysfCrcCheck(b, 6);
	if (info != NULL) {
		info->errors = (uint8_t)(cost > 255 ? 255 : cost);
		info->golay_ok = golay;
		info->crc_ok = crc;
	}
	if (f != NULL) {
		fichUnpack(b, f);
	}
	return golay && crc;
}

bool ysfFichDecodeSoft(const int8_t *soft, ysf_fich_t *f, ysf_fec_info_t *info)
{
	static int8_t conv[200];
	uint8_t words[13] = { 0 };
	softDeinterleave(soft + 8 * YSF_SYNC_BYTES, conv, 5);
	unsigned errs = convDecodeSoft(conv, 100, words, 96);
	uint16_t d[4];
	bool golay = true;
	for (int i = 0; i < 4; i++) {
		uint32_t cw = ((uint32_t)words[3 * i] << 16) | ((uint32_t)words[3 * i + 1] << 8) | words[3 * i + 2];
		if (ambeGolay24Decode(cw, &d[i]) < 0) {
			golay = false;
		}
	}
	uint8_t b[6] = {
		(uint8_t)(d[0] >> 4), (uint8_t)(((d[0] & 0x0Fu) << 4) | (d[1] >> 8)), (uint8_t)d[1],
		(uint8_t)(d[2] >> 4), (uint8_t)(((d[2] & 0x0Fu) << 4) | (d[3] >> 8)), (uint8_t)d[3],
	};
	bool crc = ysfCrcCheck(b, 6);
	if (info != NULL) {
		info->errors = (uint8_t)(errs > 255 ? 255 : errs);
		info->golay_ok = golay;
		info->crc_ok = crc;
	}
	if (f != NULL) {
		fichUnpack(b, f);
	}
	return golay && crc;
}

// --- data channels (DCH, CSD): bytes of content, whitened, CRC, rate 1/2, dibit interleaved 20 x cols, spread over
// the five payload blocks at block offset off, piece bytes each ---

static void chanEncode(const uint8_t *content, unsigned clen, unsigned cols, unsigned off, unsigned piece,
		       uint8_t *frame)
{
	uint8_t buf[23] = { 0 };	// content + CRC + tail byte (the 4 tail bits are its top bits)
	for (unsigned i = 0; i < clen; i++) {
		buf[i] = content[i] ^ whitening[i];
	}
	ysfCrcAdd(buf, clen + 2);
	unsigned nsym = 20 * cols;
	uint8_t conv[45], inter[45];
	ysfConvEncode(buf, conv, nsym);
	dibitInterleave(conv, inter, cols);
	uint8_t *p = frame + YSF_PAYLOAD_OFFSET + off;
	for (int blk = 0; blk < 5; blk++, p += 18) {
		memcpy(p, inter + blk * piece, piece);
	}
}

static bool chanDecode(const uint8_t *frame, unsigned clen, unsigned cols, unsigned off, unsigned piece,
		       uint8_t *content)
{
	uint8_t inter[45], conv[45], buf[23] = { 0 };
	const uint8_t *p = frame + YSF_PAYLOAD_OFFSET + off;
	for (int blk = 0; blk < 5; blk++, p += 18) {
		memcpy(inter + blk * piece, p, piece);
	}
	unsigned nsym = 20 * cols;
	dibitDeinterleave(inter, conv, cols);
	ysfConvDecode(conv, nsym, buf, (clen + 2) * 8);
	bool ok = ysfCrcCheck(buf, clen + 2);
	if (content != NULL) {
		for (unsigned i = 0; i < clen; i++) {
			content[i] = buf[i] ^ whitening[i];
		}
	}
	return ok;
}

static bool chanDecodeSoft(const int8_t *soft, unsigned clen, unsigned cols, unsigned off, unsigned piece,
			   uint8_t *content)
{
	static int8_t inter[360], conv[360];
	uint8_t buf[23] = { 0 };
	for (unsigned blk = 0; blk < 5; blk++) {
		memcpy(inter + blk * piece * 8, soft + (YSF_PAYLOAD_OFFSET + 18u * blk + off) * 8u, piece * 8u);
	}
	unsigned nsym = 20 * cols;
	softDeinterleave(inter, conv, cols);
	convDecodeSoft(conv, nsym, buf, (clen + 2) * 8);
	bool ok = ysfCrcCheck(buf, clen + 2);
	if (content != NULL) {
		for (unsigned i = 0; i < clen; i++) {
			content[i] = buf[i] ^ whitening[i];
		}
	}
	return ok;
}

void ysfCsdEncode(const uint8_t csd1[20], const uint8_t csd2[20], uint8_t *frame)
{
	chanEncode(csd1, 20, 9, 0, 9, frame);
	chanEncode(csd2, 20, 9, 9, 9, frame);
}

unsigned ysfCsdDecode(const uint8_t *frame, uint8_t csd1[20], uint8_t csd2[20])
{
	unsigned ok = 0;
	if (chanDecode(frame, 20, 9, 0, 9, csd1)) {
		ok |= 1u;
	}
	if (chanDecode(frame, 20, 9, 9, 9, csd2)) {
		ok |= 2u;
	}
	return ok;
}

// --- VCH (V/D mode 2) ---

// Bit interleave 26 x 4: VCH bit i goes to position (i % 26) * 4 + i / 26
static inline unsigned vchPos(unsigned i)
{
	return (i % 26u) * 4u + i / 26u;
}

void ysfVchEncode(const uint8_t ambe[YSF_AMBE_BITS], uint8_t vch[13])
{
	uint8_t raw[13] = { 0 };
	for (unsigned i = 0; i < 27; i++) {
		for (unsigned r = 0; r < 3; r++) {
			ysfPutBit(raw, 3 * i + r, ambe[i] & 1);
		}
	}
	for (unsigned i = 0; i < 22; i++) {
		ysfPutBit(raw, 81 + i, ambe[27 + i] & 1);
	}
	for (int i = 0; i < 13; i++) {
		raw[i] ^= whitening[i];
	}
	memset(vch, 0, 13);
	for (unsigned i = 0; i < 104; i++) {
		ysfPutBit(vch, vchPos(i), ysfGetBit(raw, i));
	}
}

unsigned ysfVchDecode(const uint8_t vch[13], uint8_t ambe[YSF_AMBE_BITS])
{
	uint8_t raw[13] = { 0 };
	for (unsigned i = 0; i < 104; i++) {
		ysfPutBit(raw, i, ysfGetBit(vch, vchPos(i)));
	}
	for (int i = 0; i < 13; i++) {
		raw[i] ^= whitening[i];
	}
	unsigned errs = 0;
	for (unsigned i = 0; i < 27; i++) {
		int v = ysfGetBit(raw, 3 * i) + ysfGetBit(raw, 3 * i + 1) + ysfGetBit(raw, 3 * i + 2);
		if (v == 1 || v == 2) {
			errs++;
		}
		ambe[i] = (uint8_t)(v >= 2);
	}
	for (unsigned i = 0; i < 22; i++) {
		ambe[27 + i] = (uint8_t)ysfGetBit(raw, 81 + i);
	}
	return errs;
}

unsigned ysfCsdDecodeSoft(const int8_t *soft, uint8_t csd1[20], uint8_t csd2[20])
{
	unsigned ok = 0;
	if (chanDecodeSoft(soft, 20, 9, 0, 9, csd1)) {
		ok |= 1u;
	}
	if (chanDecodeSoft(soft, 20, 9, 9, 9, csd2)) {
		ok |= 2u;
	}
	return ok;
}

// One VCH from soft values (the 104 at the frame's bit base): the three copies of bits 0-26 summed (a weighted
// majority), the rest by sign. Returns the triplets whose copies disagreed
static unsigned vchDecodeSoft(const int8_t *soft, uint8_t ambe[YSF_AMBE_BITS])
{
	int8_t raw[104];
	for (unsigned i = 0; i < 104; i++) {
		int8_t v = soft[vchPos(i)];
		raw[i] = (whitening[i / 8] >> (7 - i % 8)) & 1u ? (int8_t)-v : v;	// whitening flips the bit
	}
	unsigned errs = 0;
	for (unsigned i = 0; i < 27; i++) {
		int a = raw[3 * i], b = raw[3 * i + 1], c = raw[3 * i + 2];
		unsigned ones = (unsigned)(a > 0) + (unsigned)(b > 0) + (unsigned)(c > 0);
		if (ones == 1 || ones == 2) {
			errs++;
		}
		ambe[i] = (uint8_t)(a + b + c > 0);
	}
	for (unsigned i = 0; i < 22; i++) {
		ambe[27 + i] = (uint8_t)(raw[81 + i] > 0);
	}
	return errs;
}

bool ysfVd2DecodeSoft(const int8_t *soft, uint8_t dch[YSF_CALLSIGN_LEN], uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		      uint8_t vch_errors[YSF_VCH_PER_FRAME])
{
	bool ok = chanDecodeSoft(soft, YSF_CALLSIGN_LEN, 5, 0, 5, dch);
	if (ambe != NULL) {
		for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
			unsigned base = (YSF_PAYLOAD_OFFSET + 18u * (unsigned)j + 5u) * 8u;
			unsigned e = vchDecodeSoft(soft + base, ambe[j]);
			if (vch_errors != NULL) {
				vch_errors[j] = (uint8_t)e;
			}
		}
	}
	return ok;
}

void ysfVd2Encode(const uint8_t dch[YSF_CALLSIGN_LEN], const uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		  uint8_t *frame)
{
	chanEncode(dch, YSF_CALLSIGN_LEN, 5, 0, 5, frame);
	uint8_t vch[13];
	for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
		ysfVchEncode(ambe[j], vch);
		unsigned base = (YSF_PAYLOAD_OFFSET + 18u * (unsigned)j + 5u) * 8u;
		for (unsigned i = 0; i < 104; i++) {
			ysfPutBit(frame, base + i, ysfGetBit(vch, i));
		}
	}
}

bool ysfVd2Decode(const uint8_t *frame, uint8_t dch[YSF_CALLSIGN_LEN], uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS],
		  uint8_t vch_errors[YSF_VCH_PER_FRAME])
{
	bool ok = chanDecode(frame, YSF_CALLSIGN_LEN, 5, 0, 5, dch);
	if (ambe != NULL) {
		uint8_t vch[13];
		for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
			unsigned base = (YSF_PAYLOAD_OFFSET + 18u * (unsigned)j + 5u) * 8u;
			for (unsigned i = 0; i < 104; i++) {
				ysfPutBit(vch, i, ysfGetBit(frame, base + i));
			}
			unsigned e = ysfVchDecode(vch, ambe[j]);
			if (vch_errors != NULL) {
				vch_errors[j] = (uint8_t)e;
			}
		}
	}
	return ok;
}

// Decoded from YSF's silence VCH (7B B2 8E 43 36 E4 A2 39 78 49 33 68 33), the parameter order of ysf.h
const uint8_t ysf_ambe_silence[YSF_AMBE_BITS] = {
#include "ysf_silence.inc"
};
