// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM baseband (core/c4fm/c4fm_dsp.c): modulator -> channel (gain, DC, polarity, noise, timing offset) -> receiver,
// frames compared bit for bit and through the YSF FEC.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c4fm/c4fm_dsp.h"
#include "c4fm/ysf.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 4242;
static uint32_t rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

// Roughly Gaussian, unit variance x 1024
static int32_t gauss1024(void)
{
	int32_t s = 0;
	for (int i = 0; i < 12; i++) {
		s += (int32_t)(rnd() & 0xFFFFu);
	}
	return (s - 6 * 65536) / 64;
}

#define NFRAMES		12
#define PREAMBLE	60	// bytes of 0x77: 100 ms
static uint8_t tx[NFRAMES][YSF_FRAME_BYTES];

static void buildFrames(void)
{
	for (int f = 0; f < NFRAMES; f++) {
		uint8_t *fr = tx[f];
		memset(fr, 0, YSF_FRAME_BYTES);
		ysfAddSync(fr);
		ysf_fich_t fi = { .fi = f == 0 ? YSF_FI_HEADER : f == NFRAMES - 1 ? YSF_FI_TERMINATOR : YSF_FI_COMMS,
				  .cs = 2, .fn = (uint8_t)((f - 1) % 7), .ft = 6, .dt = YSF_DT_VD2 };
		if (f == 0 || f == NFRAMES - 1) {
			fi.fn = 0;
		}
		ysfFichEncode(&fi, fr);
		if (fi.fi == YSF_FI_COMMS) {
			uint8_t ambe[5][49];
			for (int j = 0; j < 5; j++) {
				for (int i = 0; i < 49; i++) {
					ambe[j][i] = (uint8_t)(rnd() & 1);
				}
			}
			ysfVd2Encode((const uint8_t *)"SP9ABC    ", ambe, fr);
		} else {
			ysfCsdEncode((const uint8_t *)"ALL       SP9ABC    ", (const uint8_t *)"                    ", fr);
		}
	}
}

typedef struct {
	int got, exact, fich_ok, fich_soft_ok, lost, flywheel;
	unsigned bit_errors;
	bool inverted;
} rx_t;

static void onFrame(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, void *ctx)
{
	rx_t *r = ctx;
	if (info->flywheel) {
		r->flywheel++;	// a missed sync bridged (after the end of the transmission)
		return;
	}
	// Which transmitted frame: the one with the fewest differing bits
	unsigned best = 9999;
	for (int f = 0; f < NFRAMES; f++) {
		unsigned d = 0;
		for (int i = 0; i < YSF_FRAME_BYTES; i++) {
			d += (unsigned)__builtin_popcount(frame[i] ^ tx[f][i]);
		}
		if (d < best) {
			best = d;
		}
	}
	r->got++;
	r->exact += best == 0;
	r->bit_errors += best;
	r->inverted = info->inverted;
	if (ysfFichDecode(frame, NULL, NULL)) {
		r->fich_ok++;
	}
	if (info->soft != NULL && ysfFichDecodeSoft(info->soft, NULL, NULL)) {
		r->fich_soft_ok++;
	}
}

static void onLost(void *ctx)
{
	((rx_t *)ctx)->lost++;
}

// The receiver's equalizer (c4fmDemodSetEq) and a one-pole low-pass on the channel (a repeater chain's lost highs:
// y += (x - y) * lp / 256; 0 = none), for the runs that set them
static bool eq_on;
static int chan_lp;
// A CTCSS tone added to the channel (a repeater's, under the 4FSK): amplitude, 0 none; and the receiver's tone filter
static int chan_tone;
static bool tone_on;
static uint16_t tone_found;
// The equalizer's center tap at the start (0 = as set up): a state left over from an earlier signal
static int32_t eq_stale_center;
// A noise lock just before the transmission: a sync with 3 bit errors and 40 ms of random symbols
static bool fake_lock;

static rx_t run(int amp, int dc, bool invert, int noise_rms, unsigned lead_samples)
{
	static int16_t audio[200000];
	c4fm_mod_t m;
	c4fmModInit(&m, (int16_t)amp);
	uint32_t n = 0;
	for (unsigned i = 0; i < lead_samples; i++) {
		audio[n++] = 0;
	}
	if (fake_lock) {
		uint8_t fake[5 + 24];
		memset(fake, 0, sizeof fake);
		ysfAddSync(fake);
		fake[0] ^= 0x40;
		fake[2] ^= 0x10;
		fake[4] ^= 0x04;
		for (unsigned i = 5; i < sizeof fake; i++) {
			fake[i] = (uint8_t)rnd();
		}
		n += c4fmModulate(&m, fake, sizeof fake * 8, &audio[n]);
	}
	uint8_t pre[PREAMBLE];
	memset(pre, 0x77, sizeof pre);
	n += c4fmModulate(&m, pre, PREAMBLE * 8, &audio[n]);
	for (int f = 0; f < NFRAMES; f++) {
		n += c4fmModulate(&m, tx[f], YSF_FRAME_BITS, &audio[n]);
	}
	uint8_t tail[30] = { 0 };
	n += c4fmModulate(&m, tail, sizeof tail * 8, &audio[n]);
	for (uint32_t i = 0; i < 4000; i++) {
		audio[n++] = 0;	// silence after: the receiver must give up
	}
	int32_t lp = 0;
	for (uint32_t i = 0; i < n; i++) {
		int32_t v = audio[i];
		if (chan_tone != 0) {
			v += (int32_t)(chan_tone * sin(2.0 * M_PI * 127.3 * i / 8000.0));
		}
		if (chan_lp != 0) {
			lp += ((v - lp) * chan_lp) / 256;
			v = lp;
		}
		if (invert) {
			v = -v;
		}
		v += dc + (gauss1024() * noise_rms) / 1024;
		audio[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	rx_t r = { 0 };
	c4fm_demod_t d;
	c4fmDemodInit(&d, onFrame, onLost, &r);
	c4fmDemodSetEq(&d, eq_on ? 7 : 0, 5, 6);
	c4fmDemodSetToneFilter(&d, tone_on);
	if (eq_stale_center != 0 && d.eq_taps != 0) {
		d.eq_w[d.eq_taps / 2u] = eq_stale_center;
	}
	for (uint32_t i = 0; i < n; i += 256) {
		c4fmDemodFeed(&d, &audio[i], n - i < 256 ? n - i : 256);
	}
	tone_found = c4fmDemodTone(&d);
	return r;
}

int main(void)
{
	buildFrames();

	rx_t r = run(2000, 0, false, 0, 37);
	CHECK(r.got == NFRAMES && r.exact == NFRAMES, "clean: %d frames, %d exact", r.got, r.exact);
	CHECK(r.lost == 1, "clean: lost once at the end (%d)", r.lost);
	CHECK(!r.inverted, "clean: not inverted");

	for (unsigned lead = 0; lead < 15; lead++) {
		r = run(2000, 0, false, 0, lead);
		CHECK(r.exact == NFRAMES, "timing offset %u samples: %d exact", lead, r.exact);
	}

	r = run(1500, 3000, true, 0, 11);
	CHECK(r.exact == NFRAMES && r.inverted, "inverted, DC offset: %d exact, inverted %d", r.exact, r.inverted);

	r = run(300, -500, false, 0, 5);
	CHECK(r.exact == NFRAMES, "weak signal, DC: %d exact", r.exact);

	// Noise: inner level = amp; outer = 3 amp. SNR per symbol about 20 log10(amp / rms)
	r = run(2000, 0, false, 400, 3);
	CHECK(r.got == NFRAMES && r.fich_ok == NFRAMES, "noise rms 0.2 level: %d frames, %d FICH ok, %u bit errors",
	      r.got, r.fich_ok, r.bit_errors);
	r = run(2000, 0, false, 700, 3);
	printf("noise rms 0.35 level: %d frames, %d FICH ok, %u bit errors, %d exact\n", r.got, r.fich_ok, r.bit_errors,
	       r.exact);
	CHECK(r.fich_ok >= NFRAMES - 2, "noise rms 0.35 level: FICH survives (%d)", r.fich_ok);
	// Soft decisions (as the repeater captures of 2026-11-14 showed): at noise where hard decisions lose most FICHs,
	// the soft decision Viterbi keeps nearly all
	r = run(2000, 0, false, 1000, 3);
	printf("noise rms 0.5 level: %d frames, FICH ok %d hard, %d soft\n", r.got, r.fich_ok, r.fich_soft_ok);
	CHECK(r.fich_soft_ok >= r.fich_ok + 2 && r.fich_soft_ok >= (r.got * 3) / 4,
	      "noise rms 0.5 level: soft decisions keep more FICHs (%d hard, %d soft of %d)", r.fich_ok, r.fich_soft_ok, r.got);

	// The equalizer (bench: SR5WI's highs come through a repeater chain short): on a clean signal it changes nothing;
	// on a low-passed noisy one it takes out most of the inter-symbol interference
	eq_on = true;
	r = run(2000, 0, false, 0, 37);
	CHECK(r.got == NFRAMES && r.exact == NFRAMES, "equalizer, clean: %d frames, %d exact", r.got, r.exact);
	r = run(1500, 3000, true, 0, 11);
	CHECK(r.exact == NFRAMES && r.inverted, "equalizer, inverted, DC offset: %d exact", r.exact);
	chan_lp = 110;
	eq_on = false;
	rx_t plain = run(2000, 0, false, 300, 3);
	eq_on = true;
	r = run(2000, 0, false, 300, 3);
	printf("low-passed channel: %u bit errors plain, %u equalized (%d / %d frames)\n", plain.bit_errors, r.bit_errors,
	       plain.got, r.got);
	CHECK(r.got >= plain.got && plain.bit_errors > 0 && r.bit_errors * 2 < plain.bit_errors,
	      "equalizer halves the bit errors on a low-passed channel (%u plain, %u equalized)", plain.bit_errors,
	      r.bit_errors);
	chan_lp = 0;
	// A center tap left near 0.4 (bench, 2026-11-15: a radio stuck at 0.355 decoded no FICH until a restart; there
	// every decision is +-1 and the updates keep it): a new signal starts from a plain pass-through
	eq_stale_center = (int32_t)(0.38 * (1 << 28));
	r = run(2000, 0, false, 300, 3);
	CHECK(r.got == NFRAMES && r.fich_ok == NFRAMES && r.bit_errors < 20,
	      "equalizer, stale center tap 0.38: %d frames, %d FICH ok, %u bit errors", r.got, r.fich_ok, r.bit_errors);
	eq_stale_center = 0;
	eq_on = false;

	// The tone filter (bench: SR5WI sends 127.3 Hz under its C4FM, 2026-11-14): it finds a tone of 0.4 of a level
	// in noise and takes out the errors it caused; on a signal without one it picks none
	eq_on = true;
	tone_on = true;
	r = run(2000, 0, false, 300, 3);
	CHECK(r.got == NFRAMES && tone_found == 0, "tone filter, no tone: %d frames, tone %u", r.got, tone_found);
	chan_tone = 800;
	tone_on = false;
	plain = run(2000, 0, false, 300, 3);
	tone_on = true;
	r = run(2000, 0, false, 300, 3);
	printf("127.3 Hz tone: %u bit errors plain, %u with the tone filter (found %u.%u Hz)\n", plain.bit_errors,
	       r.bit_errors, tone_found / 10u, tone_found % 10u);
	CHECK(tone_found == 1273, "tone filter finds 127.3 Hz (%u)", tone_found);
	// The first two blocks of frames still carry the tone's errors (a tone is confirmed over two, found anew each lock)
	CHECK(plain.bit_errors > 0 && r.bit_errors * 3 < plain.bit_errors * 2,
	      "tone filter takes out a third of the bit errors or more (%u plain, %u filtered)", plain.bit_errors,
	      r.bit_errors);
	chan_tone = 0;
	tone_on = false;
	eq_on = false;

	// Pure noise must not deliver frames that pass the FICH CRC
	{
		static int16_t noise[80000];
		for (int i = 0; i < 80000; i++) {
			noise[i] = (int16_t)(gauss1024() * 6000 / 1024);
		}
		rx_t z = { 0 };
		c4fm_demod_t d;
		c4fmDemodInit(&d, onFrame, onLost, &z);
		c4fmDemodFeed(&d, noise, 80000);
		CHECK(z.fich_ok == 0, "noise only: %d frames, %d with a good FICH", z.got, z.fich_ok);
		printf("noise only (10 s): %d false frames\n", z.got);
	}
	// A transmission starting 40 ms into a noise lock (a sync with 3 bit errors; receiver noise passes a first lock
	// every few minutes, SR5WI 2026-11-15): its clean sync takes over and frame 0 is received. The lock used to hold
	// the receiver for 400 ms (3 flywheel frames) and the header went by
	fake_lock = true;
	r = run(2000, 0, false, 200, 3);
	fake_lock = false;
	CHECK(r.got == NFRAMES && r.fich_ok == NFRAMES, "start during a noise lock: %d of %d frames, %d FICH ok", r.got,
	      NFRAMES, r.fich_ok);

	printf("c4fmDspTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
