// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * C4FM baseband: the receiver's symbol recovery from 8 kHz discriminator audio, and a modulator to 8 kHz audio (host
 * tests, and a transmit path through an FM modulator on boards without a 4FSK modem).
 *
 * Receive: each 8 kHz sample is interpolated by 3 through the root raised cosine matched filter (alpha 0.2, 8
 * symbols), giving 5 samples per 4800 Bd symbol at 24 kHz. Unlocked, every 24 kHz sample is correlated with the 20
 * sync symbols (DC removed, normalized by the mean absolute deviation); the best point in the 5 samples after the
 * threshold is crossed sets symbol timing and frame start. The center (DC: frequency offset) and gain come from a
 * least squares fit of the sync's sampled values to its known levels, so a negative gain (an inverted discriminator)
 * needs no special case. Locked, the next sync is looked for within +-2 samples of where it is due and the levels are
 * refit; a missed sync is bridged on the old timing up to C4FM_MAX_MISSES times, then the receiver searches again.
 * A first lock is tentative until its second sync: meanwhile a clean sync elsewhere takes over (receiver noise
 * passes a first lock every few minutes).
 *
 * Integer only (no FPU on the target). No RTOS calls: the caller feeds samples and gets frames by callback.
 */
#ifndef RFOUNDRY_C4FM_DSP_H
#define RFOUNDRY_C4FM_DSP_H

#include <stdbool.h>
#include <stdint.h>
#include "c4fm/ysf.h"

#define C4FM_SPS		5	// samples per symbol at 24 kHz
#define C4FM_RING		128	// matched filter output history (>= 20 symbols + the search window)
#define C4FM_MAX_MISSES		3
#define C4FM_EQ_MAX		15	// equalizer taps, at most
#define C4FM_TONES		50	// the standard CTCSS tones the tone filter knows (c4fmToneHz10)

typedef struct {
	uint8_t sync_errors;	// bits of the 40-bit sync received wrong
	bool inverted;		// the discriminator's polarity is reversed (negative gain)
	bool flywheel;		// this frame's sync was missed: sliced on the previous timing and levels
	int16_t gain;		// sampled value per level unit (+-1 = inner, +-3 = outer)
	int16_t center;		// sampled DC
	// Soft decisions, one per frame bit (960, the sync's included): > 0 a 1, < 0 a 0, the magnitude how far the
	// sample was from the decision threshold (32 = one level unit). The ysf*DecodeSoft decoders take them
	const int8_t *soft;
} c4fm_frame_info_t;

// A whole 120-byte frame (sync included, as ideal bits)
typedef void (*c4fm_frame_cb_t)(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, void *ctx);
// The receiver gave up the signal (C4FM_MAX_MISSES syncs missed in a row)
typedef void (*c4fm_lost_cb_t)(void *ctx);

typedef struct {
	c4fm_frame_cb_t on_frame;
	c4fm_lost_cb_t on_lost;
	void *ctx;

	int16_t x[16];		// 8 kHz input history for the polyphase filter
	uint8_t xpos;
	int16_t y[C4FM_RING];	// matched filter output at 24 kHz
	uint32_t n;		// 24 kHz sample counter (index of the newest y)

	bool locked;
	bool confirmed;		// the lock's second sync seen (a first lock alone is tentative: c4fm_dsp.c LOCK_SURE_ERRS)
	// search: candidate peak
	bool cand;
	uint32_t cand_n, cand_until;
	int64_t cand_c, cand_e;

	uint32_t sync_end;	// sample of the last sync symbol of the current frame
	uint16_t sym;		// symbols sliced after that sync (0..460)
	int32_t gain16;		// gain x 16 (signed)
	int32_t center;
	uint8_t misses;
	bool flywheel;
	// Adaptive equalizer (decision-directed LMS, c4fmDemodSetEq): taps step samples apart around the symbol instant,
	// Q28 weights. It undoes the high-frequency loss of a repeater's chain that the discriminator then passes on as
	// inter-symbol interference; 0 taps is off
	uint8_t eq_taps, eq_step, eq_mu;
	int32_t eq_w[C4FM_EQ_MAX];
	// Tone filter (c4fmDemodSetToneFilter): some repeaters send a CTCSS tone under the 4FSK, which the receiver takes
	// for level errors. The data symbols' slicing errors go through a Goertzel filter per standard tone each frame;
	// a tone well above the rest in two blocks running selects a notch on the input. Once in, the notch's own cut
	// out of the signal looks like its tone in the errors, so a choice never clears itself: each new lock starts
	// with none (a repeater's tone is found again in 0.8 s). tone_sel: 1 + the tone's index, 0 none; tone_cand: the
	// last block's winner, 0 none; tone_frames counts frames into the Goertzel block. The filters see each error less the one a frame earlier at the same
	// place: what repeats every frame (the FICH, a voice frame held, the errors a repeated symbol pattern leaves) is a
	// comb at multiples of 10 Hz that the 2.5 Hz bins took for a tone (bench: 131.8 / 199.5 Hz on a direct signal
	// with none); a tone is not periodic in the frame and stays. tone_prev_ok: tone_prev holds the previous frame's
	bool tone_on, tone_prev_ok;
	uint8_t tone_sel, tone_cand, tone_frames;
	int8_t tone_prev[YSF_FRAME_SYMBOLS - 20];	// the data symbols' errors, halved
	int32_t nx1, nx2, ny1, ny2;		// the notch's state
	int32_t g_s1[C4FM_TONES], g_s2[C4FM_TONES];
	int64_t g_acc[C4FM_TONES];		// tone power per block, leaky over about 4 blocks
	uint8_t frame[YSF_FRAME_BYTES];
	int8_t soft[YSF_FRAME_BYTES * 8];
	c4fm_frame_info_t info;

	// statistics
	uint32_t frames, syncs, losts, flywheels;
} c4fm_demod_t;

void c4fmDemodInit(c4fm_demod_t *d, c4fm_frame_cb_t on_frame, c4fm_lost_cb_t on_lost, void *ctx);
void c4fmDemodReset(c4fm_demod_t *d);
void c4fmDemodFeed(c4fm_demod_t *d, const int16_t *pcm8k, uint32_t n);
// Equalizer: taps (odd, <= C4FM_EQ_MAX; 0 off), step (24 kHz samples between taps), mu (the adaptation step, a
// right shift: larger is slower). Kept across c4fmDemodReset
void c4fmDemodSetEq(c4fm_demod_t *d, uint8_t taps, uint8_t step, uint8_t mu);
// Tone filter on or off. The selected tone is kept across c4fmDemodReset (a repeater keeps its tone); off clears it
void c4fmDemodSetToneFilter(c4fm_demod_t *d, bool on);
// The selected tone in tenths of Hz (0: none)
uint16_t c4fmDemodTone(const c4fm_demod_t *d);

// --- modulator ---

typedef struct {
	int16_t sym[9];		// recent symbol levels (-3..3) for the shaping filter
	uint8_t spos;
	uint8_t phase;		// 24 kHz sample within the symbol (0..4)
	uint8_t dec;		// decimation by 3 to 8 kHz
	int16_t amp;		// output units per level unit (inner deviation)
} c4fm_mod_t;

void c4fmModInit(c4fm_mod_t *m, int16_t amp);
// nbits bits (a multiple of 2, MSB of bytes[0] first) -> 8 kHz samples; returns the count written (nbits / 2 * 5 / 3,
// the remainder carried in the state). out must hold nbits * 5 / 6 + 2 samples
uint32_t c4fmModulate(c4fm_mod_t *m, const uint8_t *bytes, uint32_t nbits, int16_t *out);

// Level of a dibit (01 +3, 00 +1, 10 -1, 11 -3)
static inline int8_t c4fmLevel(uint8_t dibit)
{
	static const int8_t lv[4] = { 1, 3, -1, -3 };
	return lv[dibit & 3u];
}

#endif
