// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// YSF layer 2 (core/c4fm/ysf.c): bit-exact against vectors from MMDVM_CM (c4fmOracle.h), round trips, and error
// correction with injected bit errors.
#include <stdio.h>
#include <string.h>

#include "c4fm/ysf.h"
#include "c4fmOracle.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 777;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static ysf_fich_t fichFrom(const uint8_t *v)
{
	ysf_fich_t f = {
		.fi = v[0], .cs = v[1], .cm = v[2], .bn = v[3], .bt = v[4], .fn = v[5], .ft = v[6], .dev = v[7] != 0,
		.mr = v[8], .voip = v[9] != 0, .dt = v[10], .sql = v[11] != 0, .dgid = v[12],
	};
	return f;
}

static void testCrc(void)
{
	uint8_t b[8] = { 1, 2, 3, 4, 5, 6 };
	ysfCrcAdd(b, 8);
	CHECK(ysfCrcCheck(b, 8), "crc round trip");
	b[3] ^= 0x10;
	CHECK(!ysfCrcCheck(b, 8), "crc detects an error");
}

static void testConv(void)
{
	for (int t = 0; t < 50; t++) {
		uint8_t in[23] = { 0 }, enc[45], out[23] = { 0 };
		for (int i = 0; i < 22; i++) {
			in[i] = (uint8_t)rnd();
		}
		ysfConvEncode(in, enc, 180);	// 176 bits + 4 zero tail bits (in[22] = 0)
		int nerr = t % 6;
		for (int e = 0; e < nerr; e++) {
			unsigned pos = (unsigned)(e * 61 + t * 7) % 360u;	// spread out: a K=5 code corrects isolated errors
			enc[pos >> 3] ^= (uint8_t)(0x80u >> (pos & 7u));
		}
		unsigned cost = ysfConvDecode(enc, 180, out, 176);
		CHECK(memcmp(in, out, 22) == 0, "viterbi with %d errors (trial %d)", nerr, t);
		CHECK(cost == (unsigned)nerr, "path metric %u, %d errors injected", cost, nerr);
	}
}

static void testFich(void)
{
	for (int c = 0; c < ORACLE_FICH_CASES; c++) {
		uint8_t frame[YSF_FRAME_BYTES] = { 0 };
		ysf_fich_t f = fichFrom(oracle_fich_fields[c]);
		ysfFichEncode(&f, frame);
		CHECK(memcmp(frame + YSF_SYNC_BYTES, oracle_fich_bytes[c], YSF_FICH_BYTES) == 0, "FICH case %d bit-exact", c);

		ysf_fich_t d;
		ysf_fec_info_t info;
		CHECK(ysfFichDecode(frame, &d, &info) && info.errors == 0, "FICH case %d decodes clean", c);
		CHECK(memcmp(&d, &f, sizeof d) == 0, "FICH case %d fields round trip", c);

		// 8 scattered channel bit errors still decode
		for (int e = 0; e < 8; e++) {
			unsigned pos = YSF_SYNC_BYTES * 8u + (unsigned)(e * 23 + c * 3) % 200u;
			frame[pos >> 3] ^= (uint8_t)(0x80u >> (pos & 7u));
		}
		CHECK(ysfFichDecode(frame, &d, &info) && memcmp(&d, &f, sizeof d) == 0, "FICH case %d with 8 errors", c);
	}
}

static void testHeader(void)
{
	uint8_t frame[YSF_FRAME_BYTES] = { 0 };
	ysfCsdEncode(oracle_csd1, oracle_csd2, frame);
	CHECK(memcmp(frame + YSF_PAYLOAD_OFFSET, oracle_header_payload, 90) == 0, "header payload bit-exact");
	uint8_t c1[20], c2[20];
	CHECK(ysfCsdDecode(frame, c1, c2) == 3, "both CSD channels decode");
	CHECK(memcmp(c1, oracle_csd1, 20) == 0 && memcmp(c2, oracle_csd2, 20) == 0, "CSD round trip");
}

static void testVd2(void)
{
	uint8_t frame[YSF_FRAME_BYTES] = { 0 };
	ysfVd2Encode(oracle_vd2_dch, oracle_vd2_ambe, frame);
	CHECK(memcmp(frame + YSF_PAYLOAD_OFFSET, oracle_vd2_payload, 90) == 0, "V/D mode 2 payload bit-exact");

	uint8_t dch[10], ambe[5][49], errs[5];
	CHECK(ysfVd2Decode(frame, dch, ambe, errs), "DCH CRC good");
	CHECK(memcmp(dch, oracle_vd2_dch, 10) == 0, "DCH round trip");
	CHECK(memcmp(ambe, oracle_vd2_ambe, sizeof ambe) == 0, "AMBE round trip");

	// One error in each protected triplet's first copy: corrected by the majority vote
	for (int j = 0; j < 5; j++) {
		uint8_t v[13], a[49];
		ysfVchEncode(oracle_vd2_ambe[j], v);
		for (unsigned i = 0; i < 27; i++) {
			unsigned raw = 3 * i, pos = (raw % 26u) * 4u + raw / 26u;
			v[pos >> 3] ^= (uint8_t)(0x80u >> (pos & 7u));
		}
		unsigned e = ysfVchDecode(v, a);
		CHECK(e == 27 && memcmp(a, oracle_vd2_ambe[j], 49) == 0, "VCH %d corrects 27 triplets (%u)", j, e);
	}
}

static void testSilence(void)
{
	// The silence parameters must encode to YSF's own silence VCH
	static const uint8_t ref[13] = { 0x7B, 0xB2, 0x8E, 0x43, 0x36, 0xE4, 0xA2, 0x39, 0x78, 0x49, 0x33, 0x68, 0x33 };
	uint8_t v[13];
	ysfVchEncode(ysf_ambe_silence, v);
	// bit 103 (the pad) is 1 in the reference pattern but 0 in ours: compare the other 103 bits
	int diff = 0;
	for (unsigned i = 0; i < 104; i++) {
		if (i != 103 && ysfGetBit(v, (i % 26u) * 4u + i / 26u) != ysfGetBit(ref, (i % 26u) * 4u + i / 26u)) {
			diff++;
		}
	}
	CHECK(diff == 0, "silence VCH matches the reference (%d bits differ)", diff);
}

static void testSync(void)
{
	uint8_t frame[YSF_FRAME_BYTES] = { 0 };
	ysfAddSync(frame);
	CHECK(ysfSyncErrors(frame) == 0, "sync");
	frame[2] ^= 0x41;
	CHECK(ysfSyncErrors(frame) == 2, "sync errors counted");
}

int main(void)
{
	testCrc();
	testConv();
	testFich();
	testHeader();
	testVd2();
	testSilence();
	testSync();
	printf("c4fmYsfTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
