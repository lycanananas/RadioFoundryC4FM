// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM baseband receiver and modulator (c4fm_dsp.h)
#include "c4fm_dsp.h"

#include <stddef.h>
#include <string.h>

// Root raised cosine, alpha 0.2, 8 symbols at 5 samples per symbol (41 taps). RX: sum 3 x 2^14 (the 8 -> 24 kHz
// interpolation's gain of 3, so each polyphase branch has unity DC gain). TX: sum of squares 2^28.
#define NTAPS	41
static const int16_t rrc_rx[NTAPS] = {
	272, 70, -230, -495, -574, -375, 76, 616, 997, 982, 463, -457, -1452, -2059, -1833, -522, 1806, 4738, 7611,
	9706, 10473, 9706, 7611, 4738, 1806, -522, -1833, -2059, -1452, -457, 463, 982, 997, 616, 76, -375, -574, -495,
	-230, 70, 272,
};
static const int16_t rrc_tx[NTAPS] = {
	201, 52, -170, -365, -424, -277, 56, 455, 736, 725, 342, -338, -1072, -1520, -1353, -385, 1333, 3497, 5618,
	7165, 7732, 7165, 5618, 3497, 1333, -385, -1353, -1520, -1072, -338, 342, 725, 736, 455, 56, -277, -424, -365,
	-170, 52, 201,
};

#define SYNC_SYMS	20
#define DATA_SYMS	(YSF_FRAME_SYMBOLS - SYNC_SYMS)		// 460
#define FRAME_SAMPLES	(YSF_FRAME_SYMBOLS * C4FM_SPS)		// 2400
#define SEARCH		2					// +- samples around a due sync
#define LOCK_MAX_ERRS	4					// sync bit errors accepted for a first lock

// Sync levels and the correlation constants derived from them (filled once)
static int8_t sync_lv[SYNC_SYMS];
static int16_t sync_L20[SYNC_SYMS];	// 20 l - sum(l): the levels with their mean removed, times 20
static int32_t sum_l, rn, rd;		// rn = 20 sum(l^2) - sum(l)^2; rd = sum |L20|; ideal |c| / e = rn / rd
static bool tables;

static void initTables(void)
{
	if (tables) {
		return;
	}
	int32_t s = 0, s2 = 0;
	for (int k = 0; k < SYNC_SYMS; k++) {
		uint8_t dibit = (uint8_t)((ysf_sync[k / 4] >> (6 - 2 * (k % 4))) & 3u);
		sync_lv[k] = c4fmLevel(dibit);
		s += sync_lv[k];
		s2 += sync_lv[k] * sync_lv[k];
	}
	sum_l = s;
	rn = SYNC_SYMS * s2 - s * s;
	rd = 0;
	for (int k = 0; k < SYNC_SYMS; k++) {
		sync_L20[k] = (int16_t)(SYNC_SYMS * sync_lv[k] - s);
		rd += sync_L20[k] < 0 ? -sync_L20[k] : sync_L20[k];
	}
	tables = true;
}

static inline int16_t ysamp(const c4fm_demod_t *d, uint32_t n)
{
	return d->y[n & (C4FM_RING - 1u)];
}

// Correlation of the 20 symbols ending at sample n with the sync: c (signed) and e (20 x the mean absolute deviation
// sum), plus the plain sum of the samples
static void correlate(const c4fm_demod_t *d, uint32_t n, int64_t *c, int64_t *e, int32_t *sx)
{
	int32_t x[SYNC_SYMS], s = 0;
	int64_t cc = 0;
	for (int k = 0; k < SYNC_SYMS; k++) {
		x[k] = ysamp(d, n - (uint32_t)(C4FM_SPS * (SYNC_SYMS - 1 - k)));
		s += x[k];
		cc += (int64_t)sync_L20[k] * x[k];
	}
	int64_t ee = 0;
	for (int k = 0; k < SYNC_SYMS; k++) {
		int32_t v = SYNC_SYMS * x[k] - s;
		ee += v < 0 ? -v : v;
	}
	*c = cc;
	*e = ee;
	*sx = s;
}

// |c| / e >= num / den x rn / rd
static bool above(int64_t c, int64_t e, int num, int den)
{
	if (e <= 0) {
		return false;
	}
	int64_t ac = c < 0 ? -c : c;
	return ac * rd * den >= (int64_t)num * rn * e;
}

static bool better(int64_t c1, int64_t e1, int64_t c2, int64_t e2)
{
	int64_t a1 = c1 < 0 ? -c1 : c1, a2 = c2 < 0 ? -c2 : c2;
	return a1 * e2 > a2 * e1;
}

// Least squares fit of the sync samples to the levels: gain x 16 and center
static void fit(c4fm_demod_t *d, int64_t c, int32_t sx)
{
	d->gain16 = (int32_t)((16 * c) / rn);
	// center = (sum x - g sum l) / 20 with g = c / rn
	d->center = (int32_t)(((int64_t)sx * rn - c * sum_l) / ((int64_t)SYNC_SYMS * rn));
}

static int8_t clamp8(int32_t v)
{
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

// The two bits' soft values of a sample (c4fm_frame_info_t.soft): the dibit's MSB is 1 for the negative levels, its
// LSB for the outer ones, so MSB = -z and LSB = |z| - 2 in level units z, 32 to a unit
static void softBits(const c4fm_demod_t *d, int32_t y, int8_t out[2])
{
	int32_t t = y - d->center, g = d->gain16;
	if (g < 0) {
		t = -t;
		g = -g;
	}
	if (g == 0) {
		out[0] = out[1] = 0;
		return;
	}
	int32_t z32 = (int32_t)(((int64_t)t * 16 * 32) / g);	// level x 32
	out[0] = clamp8(-z32);
	out[1] = clamp8((z32 < 0 ? -z32 : z32) - 64);
}

static uint8_t slice(const c4fm_demod_t *d, int32_t y)
{
	int32_t t = y - d->center, g = d->gain16;
	if (g < 0) {
		t = -t;
		g = -g;
	}
	t *= 16;
	if (t > 2 * g) {
		return 1;	// +3
	}
	if (t > 0) {
		return 0;	// +1
	}
	if (t > -2 * g) {
		return 2;	// -1
	}
	return 3;		// -3
}

static unsigned syncErrors(const c4fm_demod_t *d, uint32_t n)
{
	unsigned errs = 0;
	for (int k = 0; k < SYNC_SYMS; k++) {
		uint8_t got = slice(d, ysamp(d, n - (uint32_t)(C4FM_SPS * (SYNC_SYMS - 1 - k))));
		uint8_t want = (uint8_t)((ysf_sync[k / 4] >> (6 - 2 * (k % 4))) & 3u);
		uint8_t x = got ^ want;
		errs += (x & 1u) + (x >> 1);
	}
	return errs;
}

static void startFrame(c4fm_demod_t *d, uint32_t sync_end, bool flywheel)
{
	d->sync_end = sync_end;
	d->sym = 0;
	d->flywheel = flywheel;
	memset(d->frame, 0, sizeof d->frame);
	ysfAddSync(d->frame);
	d->info.sync_errors = flywheel ? 0xFF : (uint8_t)syncErrors(d, sync_end);
	d->info.flywheel = flywheel;
	d->info.inverted = d->gain16 < 0;
	d->info.gain = (int16_t)(d->gain16 / 16);
	d->info.center = (int16_t)d->center;
}

static void search(c4fm_demod_t *d)
{
	int64_t c, e;
	int32_t sx;
	correlate(d, d->n, &c, &e, &sx);
	if (above(c, e, 3, 4)) {
		if (!d->cand) {
			d->cand = true;
			d->cand_until = d->n + C4FM_SPS - 1;
			d->cand_n = d->n;
			d->cand_c = c;
			d->cand_e = e;
		} else if (better(c, e, d->cand_c, d->cand_e)) {
			d->cand_n = d->n;
			d->cand_c = c;
			d->cand_e = e;
		}
	}
	if (d->cand && d->n == d->cand_until) {
		d->cand = false;
		correlate(d, d->cand_n, &c, &e, &sx);
		fit(d, c, sx);
		if (d->gain16 != 0 && syncErrors(d, d->cand_n) <= LOCK_MAX_ERRS) {
			d->locked = true;
			d->misses = 0;
			d->syncs++;
			startFrame(d, d->cand_n, false);
		}
	}
}

// The data of the frame is complete: hand it over, then look for the next sync when its window has passed
static void locked(c4fm_demod_t *d)
{
	if (d->sym < DATA_SYMS) {
		if (d->n == d->sync_end + (uint32_t)(C4FM_SPS * (d->sym + 1))) {
			unsigned bit = 2u * (SYNC_SYMS + d->sym);
			int32_t y = ysamp(d, d->n);
			uint8_t dibit = slice(d, y);
			ysfPutBit(d->frame, bit, dibit >> 1);
			ysfPutBit(d->frame, bit + 1, dibit & 1);
			softBits(d, y, &d->soft[bit]);
			if (++d->sym == DATA_SYMS) {
				d->frames++;
				d->flywheels += d->flywheel;
				for (unsigned k = 0; k < 2u * SYNC_SYMS; k++) {
					d->soft[k] = ysfGetBit(d->frame, k) ? 127 : -127;	// the sync: as sent
				}
				d->info.soft = d->soft;
				if (d->on_frame != NULL) {
					d->on_frame(d->frame, &d->info, d->ctx);
				}
			}
		}
		return;
	}
	uint32_t due = d->sync_end + FRAME_SAMPLES;
	if (d->n != due + SEARCH) {
		return;
	}
	int64_t bc = 0, be = 1;
	int32_t bsx = 0;
	uint32_t bn = due;
	for (uint32_t m = due - SEARCH; m <= due + SEARCH; m++) {
		int64_t c, e;
		int32_t sx;
		correlate(d, m, &c, &e, &sx);
		if ((c < 0) == (d->gain16 < 0) && better(c, e, bc, be)) {
			bc = c;
			be = e;
			bsx = sx;
			bn = m;
		}
	}
	// The sync's own shape and a level within 6 dB of the last one: a carrier with no modulation after the end (or
	// filter ringing) correlates well but at a fraction of the level
	int32_t g_new = (int32_t)((16 * bc) / rn), g_old = d->gain16;
	int32_t an = g_new < 0 ? -g_new : g_new, ao = g_old < 0 ? -g_old : g_old;
	if (above(bc, be, 1, 2) && 2 * an >= ao && an <= 2 * ao) {
		fit(d, bc, bsx);
		d->misses = 0;
		d->syncs++;
		startFrame(d, bn, false);
		return;
	}
	if (++d->misses > C4FM_MAX_MISSES) {
		d->locked = false;
		d->losts++;
		if (d->on_lost != NULL) {
			d->on_lost(d->ctx);
		}
		return;
	}
	startFrame(d, due, true);
}

void c4fmDemodReset(c4fm_demod_t *d)
{
	c4fm_frame_cb_t f = d->on_frame;
	c4fm_lost_cb_t l = d->on_lost;
	void *ctx = d->ctx;
	memset(d, 0, sizeof *d);
	d->on_frame = f;
	d->on_lost = l;
	d->ctx = ctx;
	d->n = 0xFFFFFFFFu;	// the first sample is n = 0
}

void c4fmDemodInit(c4fm_demod_t *d, c4fm_frame_cb_t on_frame, c4fm_lost_cb_t on_lost, void *ctx)
{
	initTables();
	d->on_frame = on_frame;
	d->on_lost = on_lost;
	d->ctx = ctx;
	c4fmDemodReset(d);
}

void c4fmDemodFeed(c4fm_demod_t *d, const int16_t *pcm8k, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		d->xpos = (uint8_t)((d->xpos + 1u) & 15u);
		d->x[d->xpos] = pcm8k[i];
		for (int p = 0; p < 3; p++) {
			int32_t acc = 0;
			for (int j = 0; 3 * j + p < NTAPS; j++) {
				acc += (int32_t)rrc_rx[3 * j + p] * d->x[(d->xpos - j) & 15];
			}
			acc >>= 14;
			if (acc > 32767) {
				acc = 32767;
			} else if (acc < -32768) {
				acc = -32768;
			}
			d->n++;
			d->y[d->n & (C4FM_RING - 1u)] = (int16_t)acc;
			if (d->locked) {
				locked(d);
			} else {
				search(d);
			}
		}
	}
}

// --- modulator ---

void c4fmModInit(c4fm_mod_t *m, int16_t amp)
{
	memset(m, 0, sizeof *m);
	m->amp = amp;
}

uint32_t c4fmModulate(c4fm_mod_t *m, const uint8_t *bytes, uint32_t nbits, int16_t *out)
{
	uint32_t w = 0;
	for (uint32_t b = 0; b + 1 < nbits; b += 2) {
		uint8_t dibit = (uint8_t)((ysfGetBit(bytes, b) << 1) | ysfGetBit(bytes, b + 1));
		m->spos = (uint8_t)((m->spos + 1u) % 9u);
		m->sym[m->spos] = c4fmLevel(dibit);
		for (int p = 0; p < C4FM_SPS; p++) {
			if (m->dec == 0) {
				int32_t acc = 0;
				for (int j = 0; C4FM_SPS * j + p < NTAPS; j++) {
					acc += (int32_t)rrc_tx[C4FM_SPS * j + p] * m->sym[(m->spos + 9u - (unsigned)j) % 9u];
				}
				acc = (acc * m->amp) >> 14;
				out[w++] = (int16_t)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
			}
			m->dec = (uint8_t)((m->dec + 1u) % 3u);
		}
	}
	return w;
}
