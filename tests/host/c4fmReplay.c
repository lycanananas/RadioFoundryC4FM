// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// c4fmReplay: the radio's C4FM receiver on a capture (tools/c4fm_capture.py: int16 little endian, 8 kHz), on the host,
// bit for bit as the radio runs it (core/c4fm/c4fm_dsp.c, ysf.c). Per frame: sync errors, levels, FICH and its FEC,
// the voice triplets corrected; then totals and the bit error estimates. A helper for the bench, not a test.
//
//   c4fmReplay FILE.raw [--soft] [--eq TAPS,STEP,MU] [--tone] [--pre A] [--gain G] [--quiet]
//
// --soft: the soft decision decoders (ysf*DecodeSoft) instead of the hard ones.
//
// --eq: the receiver's adaptive equalizer (c4fmDemodSetEq): taps, step in 24 kHz samples, mu shift.
// --tone: the tone filter (c4fmDemodSetToneFilter): finds a repeater's CTCSS tone and notches it out.
//
// --pre A: a first-order pre-emphasis before the receiver, y = (x - A x[-1]) / (1 - A), 0 <= A < 1: undoes a
// de-emphasis in the radio's audio path, to see whether the frames come out cleaner. --gain G scales the input.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "c4fm/c4fm_dsp.h"
#include "c4fm/ysf.h"

static unsigned frames, fich_ok, fich_fec, comms, vch_fixed, sync_err, flywheels;
static int quiet, soft_dec;
static unsigned dch_ok;

static void on_frame(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, void *ctx)
{
	(void)ctx;
	ysf_fich_t f;
	ysf_fec_info_t fec;
	bool ok = soft_dec && info->soft ? ysfFichDecodeSoft(info->soft, &f, &fec) : ysfFichDecode(frame, &f, &fec);
	frames++;
	sync_err += info->sync_errors;
	flywheels += info->flywheel;
	unsigned vch = 0;
	if (ok) {
		fich_ok++;
		fich_fec += fec.errors;
	}
	if (ok && f.fi == YSF_FI_COMMS && f.dt == YSF_DT_VD2) {
		uint8_t dch[YSF_CALLSIGN_LEN], ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS], errs[YSF_VCH_PER_FRAME];
		bool dok = soft_dec && info->soft ? ysfVd2DecodeSoft(info->soft, dch, ambe, errs) : ysfVd2Decode(frame, dch, ambe, errs);
		dch_ok += dok;
		for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
			vch += errs[j];
		}
		vch_fixed += vch;
		comms++;
	}
	if (!quiet) {
		printf("frame %2u: sync err %2u%s gain %5d center %6d  fich %s", frames, info->sync_errors,
		       info->flywheel ? " (flywheel)" : "", info->gain, info->center, ok ? "ok " : "BAD");
		if (ok) {
			printf(" fi %u fn %u dt %u fec %2u", f.fi, f.fn, f.dt, fec.errors);
		}
		if (vch) {
			printf("  vch fixed %u", vch);
		}
		printf("\n");
	}
}

static void on_lost(void *ctx)
{
	(void)ctx;
	if (!quiet) {
		printf("-- lost\n");
	}
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: c4fmReplay FILE.raw [--soft] [--eq TAPS,STEP,MU] [--tone] [--pre A] [--gain G] [--quiet]\n");
		return 2;
	}
	double pre = 0, gain = 1;
	int eq_taps = 0, eq_step = 5, eq_mu = 6, tone = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--pre") == 0 && i + 1 < argc) {
			pre = atof(argv[++i]);
		} else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
			gain = atof(argv[++i]);
		} else if (strcmp(argv[i], "--eq") == 0 && i + 1 < argc) {
			sscanf(argv[++i], "%d,%d,%d", &eq_taps, &eq_step, &eq_mu);
		} else if (strcmp(argv[i], "--tone") == 0) {
			tone = 1;
		} else if (strcmp(argv[i], "--soft") == 0) {
			soft_dec = 1;
		} else if (strcmp(argv[i], "--quiet") == 0) {
			quiet = 1;
		}
	}
	FILE *fp = fopen(argv[1], "rb");
	if (fp == NULL) {
		perror(argv[1]);
		return 2;
	}
	static int16_t in[1 << 20], out[1 << 20];
	size_t n = fread(in, sizeof in[0], sizeof in / sizeof in[0], fp);
	fclose(fp);
	double prev = 0;
	for (size_t i = 0; i < n; i++) {
		double x = in[i];
		double y = pre > 0 ? (x - pre * prev) / (1.0 - pre) : x;
		prev = x;
		y *= gain;
		out[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
	}
	c4fm_demod_t d;
	c4fmDemodInit(&d, on_frame, on_lost, NULL);
	c4fmDemodSetEq(&d, (uint8_t)eq_taps, (uint8_t)eq_step, (uint8_t)eq_mu);
	c4fmDemodSetToneFilter(&d, tone != 0);
	c4fmDemodFeed(&d, out, (uint32_t)n);
	unsigned good = fich_ok ? fich_ok : 1, cm = comms ? comms : 1;
	printf("%s: %zu samples, %u frames (%u flywheel), fich ok %u, sync err avg %.2f, ber fich %.3f%%, ber vch %.3f%% "
	       "(%u triplets fixed in %u voice frames), dch crc ok %u/%u\n", argv[1], n, frames, flywheels, fich_ok,
	       frames ? (double)sync_err / frames : 0.0, 100.0 * fich_fec / (200.0 * good),
	       100.0 * vch_fixed / (81.0 * 5 * cm), vch_fixed, 5 * comms, dch_ok, comms);
	if (tone) {
		printf("tone filter: %u.%u Hz\n", c4fmDemodTone(&d) / 10u, c4fmDemodTone(&d) % 10u);
	}
	return 0;
}
