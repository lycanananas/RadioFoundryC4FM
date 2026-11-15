// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM baseband receiver and modulator (c4fm_dsp.h)
#include "c4fm_dsp.h"

#include <stddef.h>
#include <stdint.h>
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
// A first lock is tentative until the next sync turns up where it is due: meanwhile a sync with at most LOCK_SURE_ERRS
// errors elsewhere takes over. Receiver noise passes LOCK_MAX_ERRS every 3-5 minutes (SR5WI, 2026-11-15: 45 in 3 h),
// and a noise lock held the receiver for 400 ms (the lock and 3 flywheel frames) where a real transmission's start
// went by. Not a stricter first lock: 3 errors cost a low-passed channel half its frames, and such noise correlates
// over 0.75-1 like a weak signal. Not an end at the first missed sync either: a weak signal (a bench capture) loses
// its second sync, then tracks at 6-10 sync errors that a new first lock would never take. Noise passes
// LOCK_SURE_ERRS twice in 10 h simulated
#define LOCK_SURE_ERRS	2

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

// --- tone filter ---

// The standard CTCSS tones: tenths of Hz, the Goertzel coefficient at the symbol rate (2 cos(2 pi f / 4800), Q14),
// the input notch at 8 kHz (Q 20; b0 = b2, a1 = b1; Q29), generated with scipy.signal.iirnotch, and the power
// weight that undoes the frame difference's gain on noise there (256 / max(4 sin^2(pi f 0.1 s), 1): near a multiple
// of 10 Hz the difference leaves little of anything, and those bins keep their power as it is)
static const struct {
	uint16_t hz10;
	int16_t g;
	int32_t b0, b1, a2;
	uint16_t w;
} tones[C4FM_TONES] = {
	{ 670, 32642, 536165563, -1070846805, 535460215, 98 },
	{ 693, 32633, 536141383, -1070694883, 535411853, 256 },
	{ 719, 32623, 536114051, -1070518955, 535357190, 203 },
	{ 744, 32613, 536087773, -1070345601, 535304633, 66 },
	{ 770, 32602, 536060446, -1070160953, 535249980, 98 },
	{ 797, 32590, 536032072, -1069964501, 535193231, 256 },
	{ 825, 32577, 536002649, -1069755712, 535134387, 128 },
	{ 854, 32563, 535972180, -1069534038, 535073447, 65 },
	{ 885, 32548, 535939612, -1069290968, 535008313, 256 },
	{ 915, 32533, 535908099, -1069049733, 534945287, 256 },
	{ 948, 32516, 535873439, -1068777552, 534875967, 64 },
	{ 974, 32502, 535846135, -1068558075, 534821357, 120 },
	{ 1000, 32488, 535818833, -1068334164, 534766754, 256 },
	{ 1035, 32468, 535782085, -1068025748, 534693257, 81 },
	{ 1072, 32446, 535743242, -1067690980, 534615572, 108 },
	{ 1109, 32423, 535704405, -1067347244, 534537898, 256 },
	{ 1148, 32399, 535663475, -1066975226, 534456038, 64 },
	{ 1188, 32373, 535621502, -1066583329, 534372091, 256 },
	{ 1230, 32344, 535577437, -1066160575, 534283962, 98 },
	{ 1273, 32314, 535532330, -1065715810, 534193749, 114 },
	{ 1318, 32282, 535485134, -1065237423, 534099356, 223 },
	{ 1365, 32246, 535435849, -1064723660, 534000786, 81 },
	{ 1413, 32209, 535385524, -1064184090, 533900137, 256 },
	{ 1462, 32170, 535334161, -1063617785, 533797411, 74 },
	{ 1514, 32127, 535279664, -1062999700, 533688417, 256 },
	{ 1567, 32081, 535224131, -1062351614, 533577349, 86 },
	{ 1598, 32054, 535191654, -1061964076, 533512396, 256 },
	{ 1622, 32032, 535166514, -1061659755, 533462115, 158 },
	{ 1655, 32002, 535131949, -1061235201, 533392986, 66 },
	{ 1679, 31980, 535106814, -1060921992, 533342717, 170 },
	{ 1713, 31948, 535071211, -1060471877, 533271509, 256 },
	{ 1738, 31924, 535045034, -1060136125, 533219157, 74 },
	{ 1773, 31889, 535008392, -1059659260, 533145872, 114 },
	{ 1799, 31864, 534981175, -1059299877, 533091438, 256 },
	{ 1835, 31827, 534943495, -1058795038, 533016077, 81 },
	{ 1862, 31799, 534915238, -1058410901, 532959564, 74 },
	{ 1899, 31761, 534876521, -1057876827, 532882129, 256 },
	{ 1928, 31730, 534846178, -1057452038, 532821445, 108 },
	{ 1966, 31689, 534806425, -1056887189, 532741938, 83 },
	{ 1995, 31657, 534776091, -1056449842, 532681269, 256 },
	{ 2035, 31612, 534734256, -1055837695, 532597600, 81 },
	{ 2065, 31578, 534702884, -1055371809, 532534856, 81 },
	{ 2107, 31530, 534658970, -1054709818, 532447028, 256 },
	{ 2181, 31442, 534581615, -1053515802, 532292317, 203 },
	{ 2257, 31348, 534502192, -1052252839, 532133471, 67 },
	{ 2291, 31306, 534466668, -1051675811, 532062424, 256 },
	{ 2336, 31248, 534419659, -1050900686, 531968405, 78 },
	{ 2418, 31140, 534334018, -1049454844, 531797125, 223 },
	{ 2503, 31025, 534245274, -1047910656, 531619635, 256 },
	{ 2541, 30972, 534205609, -1047205370, 531540306, 69 },
};

// The Goertzel filters run over TONE_BLOCK frames in symbol time (the syncs and flywheel frames as zeros, so the
// phase carries on): 1920 symbols resolve 2.5 Hz, enough for tones 3-5 Hz apart, where one frame (10 Hz) is not
#define TONE_BLOCK	4
#define TONE_RATIO	6	// the tone's power over the mean of the others (not its neighbors, which its own leak lifts)

void c4fmDemodSetToneFilter(c4fm_demod_t *d, bool on)
{
	d->tone_on = on;
	if (!on) {
		d->tone_sel = 0;
	}
	d->nx1 = d->nx2 = d->ny1 = d->ny2 = 0;
}

uint16_t c4fmDemodTone(const c4fm_demod_t *d)
{
	return d->tone_sel != 0 ? tones[d->tone_sel - 1u].hz10 : 0;
}

static int16_t notch(c4fm_demod_t *d, int16_t x)
{
	if (d->tone_sel == 0) {
		return x;
	}
	const unsigned k = d->tone_sel - 1u;
	int64_t acc = (int64_t)tones[k].b0 * (x + d->nx2) + (int64_t)tones[k].b1 * (d->nx1 - d->ny1) -
		      (int64_t)tones[k].a2 * d->ny2;
	int32_t y = (int32_t)(acc >> 29);
	d->nx2 = d->nx1;
	d->nx1 = x;
	d->ny2 = d->ny1;
	d->ny1 = y;
	return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
}

static void toneStep(c4fm_demod_t *d, int32_t e)
{
	for (unsigned k = 0; k < C4FM_TONES; k++) {
		int32_t s0 = e + (int32_t)(((int64_t)tones[k].g * d->g_s1[k]) >> 14) - d->g_s2[k];
		d->g_s2[k] = d->g_s1[k];
		d->g_s1[k] = s0;
	}
}

// The sync's 20 symbols: zeros, so the filters keep their phase across the frame boundary
static void toneFrameStart(c4fm_demod_t *d)
{
	if (!d->tone_on) {
		return;
	}
	for (unsigned i = 0; i < SYNC_SYMS; i++) {
		toneStep(d, 0);
	}
}

// One data symbol's slicing error (64 to a level unit) less the previous frame's at the same place, into every tone's
// Goertzel filter (zero on a flywheel frame and on the first frame after a lock or a flywheel: no reference)
static void toneSymbol(c4fm_demod_t *d, int32_t y, uint8_t dibit)
{
	if (!d->tone_on) {
		return;
	}
	int32_t e = 0, q = 0;
	if (!d->flywheel && d->gain16 != 0) {
		int32_t want = d->center + (int32_t)(c4fmLevel(dibit) * (d->gain16 / 16));
		e = (int32_t)(((int64_t)(y - want) * 64 * 16) / d->gain16);
		q = e / 2;
		q = q > 127 ? 127 : q < -127 ? -127 : q;
	}
	int32_t diff = d->tone_prev_ok && !d->flywheel ? 2 * (q - d->tone_prev[d->sym]) : 0;
	d->tone_prev[d->sym] = (int8_t)q;
	toneStep(d, diff);
}

// Every TONE_BLOCK frames: the block's tone powers into the leaky sums; a tone well above the others becomes the notch
static void toneFrameEnd(c4fm_demod_t *d)
{
	d->tone_prev_ok = !d->flywheel;
	if (!d->tone_on || ++d->tone_frames % TONE_BLOCK != 0) {
		return;
	}
	int64_t total = 0;
	unsigned best = 0;
	for (unsigned k = 0; k < C4FM_TONES; k++) {
		int64_t s1 = d->g_s1[k], s2 = d->g_s2[k];
		int64_t pw = s1 * s1 + s2 * s2 - ((tones[k].g * s1 * s2) >> 14);
		pw = pw < 0 ? 0 : (pw >> 8) * tones[k].w;
		d->g_acc[k] += pw - d->g_acc[k] / 4;
		total += d->g_acc[k];
		if (d->g_acc[k] > d->g_acc[best]) {
			best = k;
		}
	}
	memset(d->g_s1, 0, sizeof d->g_s1);
	memset(d->g_s2, 0, sizeof d->g_s2);
	if (d->tone_sel == best + 1u) {
		d->tone_cand = 0;
		return;
	}
	int64_t rest = total;
	unsigned n = C4FM_TONES;
	for (unsigned k = best == 0 ? 0 : best - 1u; k <= best + 1u && k < C4FM_TONES; k++) {
		rest -= d->g_acc[k];
		n--;
	}
	// A neighbor of the tone in use (3-5 Hz away: its leak is large on a short block) has to be twice as strong to
	// take over: the bench saw SR5WI's 127.3 Hz flip to 131.8 Hz and back on a 0.5 s fragment
	if (d->tone_sel != 0) {
		unsigned cur = d->tone_sel - 1u;
		if ((best + 1u == cur || cur + 1u == best) && d->g_acc[best] < 2 * d->g_acc[cur]) {
			d->tone_cand = 0;
			return;
		}
	}
	if (rest <= 0 || d->g_acc[best] * n < (int64_t)TONE_RATIO * rest) {
		d->tone_cand = 0;
		return;
	}
	// One block's winner can be noise (the bench's test signal picked 67 Hz from one block); the second block in a row
	// confirms it
	if (d->tone_cand != best + 1u) {
		d->tone_cand = (uint8_t)(best + 1u);
		return;
	}
	d->tone_cand = 0;
	d->tone_sel = (uint8_t)(best + 1u);
	d->nx1 = d->nx2 = d->ny1 = d->ny2 = 0;
}

// A new signal: no tone, nothing learned (toneFrameEnd: why a choice can't be carried over)
static void toneRestart(c4fm_demod_t *d)
{
	d->tone_sel = d->tone_cand = d->tone_frames = 0;
	d->tone_prev_ok = false;
	d->nx1 = d->nx2 = d->ny1 = d->ny2 = 0;
	memset(d->g_s1, 0, sizeof d->g_s1);
	memset(d->g_s2, 0, sizeof d->g_s2);
	memset(d->g_acc, 0, sizeof d->g_acc);
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
	toneFrameStart(d);
}

static void eqIdentity(c4fm_demod_t *d)
{
	memset(d->eq_w, 0, sizeof d->eq_w);
	d->eq_w[d->eq_taps / 2u] = 1 << 28;
}

// tentative: a first lock is waiting for its next sync; only a sure sync away from that lock's timing replaces it
static void search(c4fm_demod_t *d, bool tentative)
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
		if (tentative) {
			// Not the lock's own sync (the tail of its peak) nor its next one (locked() takes that)
			uint32_t off = d->cand_n - d->sync_end;
			if (off <= SEARCH + C4FM_SPS ||
			    (off + SEARCH + C4FM_SPS >= FRAME_SAMPLES && off <= FRAME_SAMPLES + SEARCH + C4FM_SPS)) {
				return;
			}
		}
		int32_t g = d->gain16, ctr = d->center;
		correlate(d, d->cand_n, &c, &e, &sx);
		fit(d, c, sx);
		if (d->gain16 != 0 && syncErrors(d, d->cand_n) <= (tentative ? LOCK_SURE_ERRS : LOCK_MAX_ERRS)) {
			d->locked = true;
			d->confirmed = false;
			d->misses = 0;
			d->syncs++;
			eqIdentity(d);	// a new signal: nothing learned from the last one carries over
			toneRestart(d);
			startFrame(d, d->cand_n, false);
		} else if (tentative) {
			d->gain16 = g;	// keep the lock's levels
			d->center = ctr;
		}
	}
}

// --- equalizer ---

void c4fmDemodSetEq(c4fm_demod_t *d, uint8_t taps, uint8_t step, uint8_t mu)
{
	if (taps > C4FM_EQ_MAX) {
		taps = C4FM_EQ_MAX;
	}
	d->eq_taps = taps == 0 ? 0 : (uint8_t)(taps | 1u);
	d->eq_step = step == 0 ? 1 : step;
	d->eq_mu = mu;
	eqIdentity(d);
}

// The equalized sample for the symbol at m: the taps around it, the DC (center) left alone
static int32_t equalize(const c4fm_demod_t *d, uint32_t m)
{
	if (d->eq_taps == 0) {
		return ysamp(d, m);
	}
	int64_t acc = 0;
	uint32_t first = m - (uint32_t)(d->eq_taps / 2u) * d->eq_step;
	for (unsigned k = 0; k < d->eq_taps; k++) {
		acc += (int64_t)d->eq_w[k] * (ysamp(d, first + k * d->eq_step) - d->center);
	}
	int32_t y = d->center + (int32_t)(acc >> 28);
	return y > 32767 ? 32767 : y < -32768 ? -32768 : y;
}

// Normalized LMS towards the decided level: w += e x / (taps P) / 2^mu, P the signal's power at this gain (the four
// levels' mean square is 5). The center tap stays at 1: the sync's fit already sets the gain, and a free center tap
// has a second resting point near 0.4 where the outer levels fall on the inner ones, every decision is +-1 and the
// updates keep it there (the bench: a radio stuck at 0.355, no FICH until a restart). Not on flywheel frames (the
// decisions are guesses); back to a plain pass-through if a side tap runs away
#define EQ_CENTER_MIN	((7 << 28) / 10)
#define EQ_CENTER_MAX	((6 << 28) / 5)
#define EQ_SIDE_MAX	(1 << 27)	// 0.5: the repeater chains seen need under 0.15
static void adapt(c4fm_demod_t *d, uint32_t m, int32_t y, uint8_t dibit)
{
	if (d->eq_taps == 0 || d->flywheel || d->gain16 == 0) {
		return;
	}
	int64_t g = d->gain16 / 16;
	int32_t want = d->center + (int32_t)(c4fmLevel(dibit) * g);
	int64_t den = (int64_t)d->eq_taps * 5 * g * g;
	if (den <= 0) {
		return;
	}
	int64_t sc = ((int64_t)(want - y) << (36 - d->eq_mu)) / den;	// Q36 per sample unit
	uint32_t first = m - (uint32_t)(d->eq_taps / 2u) * d->eq_step;
	for (unsigned k = 0; k < d->eq_taps; k++) {
		int64_t w = d->eq_w[k] + ((sc * (ysamp(d, first + k * d->eq_step) - d->center)) >> 8);
		if (k == d->eq_taps / 2u) {
			d->eq_w[k] = (int32_t)(w < EQ_CENTER_MIN ? EQ_CENTER_MIN : w > EQ_CENTER_MAX ? EQ_CENTER_MAX : w);
			continue;
		}
		if (w > EQ_SIDE_MAX || w < -EQ_SIDE_MAX) {
			eqIdentity(d);
			return;
		}
		d->eq_w[k] = (int32_t)w;
	}
}

// The data of the frame is complete: hand it over, then look for the next sync when its window has passed
static void locked(c4fm_demod_t *d)
{
	if (d->sym < DATA_SYMS) {
		uint32_t delay = (uint32_t)(d->eq_taps / 2u) * d->eq_step;
		if (d->n == d->sync_end + (uint32_t)(C4FM_SPS * (d->sym + 1)) + delay) {
			unsigned bit = 2u * (SYNC_SYMS + d->sym);
			int32_t y = equalize(d, d->n - delay);
			uint8_t dibit = slice(d, y);
			adapt(d, d->n - delay, y, dibit);
			toneSymbol(d, y, dibit);
			ysfPutBit(d->frame, bit, dibit >> 1);
			ysfPutBit(d->frame, bit + 1, dibit & 1);
			softBits(d, y, &d->soft[bit]);
			if (++d->sym == DATA_SYMS) {
				toneFrameEnd(d);
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
		d->confirmed = true;
		d->cand = false;	// a candidate the tentative search had open would be stale by the next search
		startFrame(d, bn, false);
		return;
	}
	if (++d->misses > C4FM_MAX_MISSES) {
		d->locked = false;
		d->cand = false;
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
	uint8_t taps = d->eq_taps, step = d->eq_step, mu = d->eq_mu;
	bool tone_on = d->tone_on;
	memset(d, 0, sizeof *d);
	d->on_frame = f;
	d->on_lost = l;
	d->ctx = ctx;
	c4fmDemodSetEq(d, taps, step, mu);
	c4fmDemodSetToneFilter(d, tone_on);
	d->n = 0xFFFFFFFFu;	// the first sample is n = 0
}

void c4fmDemodInit(c4fm_demod_t *d, c4fm_frame_cb_t on_frame, c4fm_lost_cb_t on_lost, void *ctx)
{
	initTables();
	memset(d, 0, sizeof *d);
	d->on_frame = on_frame;
	d->on_lost = on_lost;
	d->ctx = ctx;
	c4fmDemodReset(d);
}

void c4fmDemodFeed(c4fm_demod_t *d, const int16_t *pcm8k, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		d->xpos = (uint8_t)((d->xpos + 1u) & 15u);
		d->x[d->xpos] = notch(d, pcm8k[i]);
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
			if (!d->locked) {
				search(d, false);
			} else {
				locked(d);
				if (d->locked && !d->confirmed) {
					search(d, true);
				}
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
