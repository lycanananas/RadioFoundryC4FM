// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The c4fm view sections (docs/spec/c4fm.md 8): part 0 the call engine's, the receiver's and the driver's counters;
// part 1 the last call's fields in hex, byte for byte as another radio sent them (tools/c4fm_bench.py decodes them).
// Two sections so each fits one reply.
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#if CONFIG_C4FM
#include "c4fm/c4fm_call.h"
#endif

#if CONFIG_C4FM
// key=hex of n bytes, or key=- when the part didn't arrive with a good CRC
static void hexField(dl_text_t *t, const char *key, const uint8_t *b, unsigned n, bool ok)
{
	char line[64];
	unsigned k = 0;
	if (ok) {
		static const char hex[] = "0123456789abcdef";
		for (unsigned i = 0; i < n && k + 2 < sizeof line; i++) {
			line[k++] = hex[b[i] >> 4];
			line[k++] = hex[b[i] & 15u];
		}
	} else {
		line[k++] = '-';
	}
	line[k] = '\0';
	dl_text(t, "%s=%s\n", key, line);
}

static void counters(dl_text_t *t)
{
	const c4fm_call_view_t *c = c4fm_call_view();
	const c4fm_config_t *cfg = c4fm_call_config();
	dl_text_kv_str(t, "c4fm.own_call", cfg->callsign);
	dl_text(t, "c4fm.own_dgid=%u\n", cfg->dgid);
	dl_text_kv_str(t, "c4fm.own_id", cfg->radio_id);
	dl_text(t, "c4fm.phase=%u\nc4fm.dgid=%u\nc4fm.dt=%u\nc4fm.late=%u\n", c->phase, c->dgid, c->dt, c->late_entry);
	dl_text_kv_str(t, "c4fm.src", c->src);
	dl_text_kv_str(t, "c4fm.dst", c->dst);
	dl_text_kv_str(t, "c4fm.src_id", c->src_id);
	dl_text(t, "c4fm.rx_fich_ok=%u\n", (unsigned)c->rx_fich_ok);
	dl_text(t, "c4fm.rx_frames=%u\nc4fm.rx_fich_bad=%u\nc4fm.rx_fich_fec=%u\nc4fm.rx_calls=%u\nc4fm.rx_vch_errors=%u\n"
		"c4fm.rx_ambe=%u\n", (unsigned)c->rx_frames, (unsigned)c->rx_fich_bad, (unsigned)c->rx_fich_fec,
		(unsigned)c->rx_calls, (unsigned)c->rx_vch_errors, (unsigned)c->rx_ambe);
	dl_text(t, "c4fm.dsp_syncs=%u\nc4fm.dsp_losts=%u\nc4fm.dsp_flywheels=%u\nc4fm.rx_gain=%d\nc4fm.rx_center=%d\n"
		"c4fm.rx_inverted=%u\n", (unsigned)c->dsp_syncs, (unsigned)c->dsp_losts, (unsigned)c->dsp_flywheels,
		c->rx_gain, c->rx_center, c->rx_inverted);
	dl_text(t, "c4fm.tx_calls=%u\nc4fm.tx_frames=%u\nc4fm.tx_padded=%u\n", (unsigned)c->tx_calls,
		(unsigned)c->tx_frames, (unsigned)c->tx_padded);
	if (board.c4fm != NULL && board.c4fm->stats != NULL) {
		hal_c4fm_stats_t s;
		board.c4fm->stats(&s);
		dl_text(t, "c4fm.drv_keyups=%u\nc4fm.drv_slots=%u\nc4fm.drv_sent=%u\nc4fm.drv_idle=%u\nc4fm.drv_stalls=%u\n",
			(unsigned)s.keyups, (unsigned)s.slots, (unsigned)s.sent, (unsigned)s.idle, (unsigned)s.stalls);
		dl_text(t, "c4fm.drv_ints=%u,%u,%u\nc4fm.drv_level=%u\nc4fm.drv_regs=%x,%x\n", (unsigned)s.ints[0],
			(unsigned)s.ints[1], (unsigned)s.ints[2], (unsigned)s.level_pct, (unsigned)s.regs[0],
			(unsigned)s.regs[1]);
	}
}

static void rawFields(dl_text_t *t)
{
	const c4fm_raw_t *r = c4fm_call_raw();
	const ysf_fich_t *f = &r->fich;
	dl_text(t, "c4fm.raw_serial=%u\n", (unsigned)r->serial);
	// fi,cs,cm,bn,bt,fn,ft,dev,mr,voip,dt,sql,dgid
	dl_text(t, "c4fm.raw_fich=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", f->fi, f->cs, f->cm, f->bn, f->bt, f->fn,
		f->ft, f->dev, f->mr, f->voip, f->dt, f->sql, f->dgid);
	hexField(t, "c4fm.raw_hdr_csd1", r->hdr_csd1, 20, (r->hdr_ok & 1u) != 0);
	hexField(t, "c4fm.raw_hdr_csd2", r->hdr_csd2, 20, (r->hdr_ok & 2u) != 0);
	hexField(t, "c4fm.raw_term_csd1", r->term_csd1, 20, (r->term_ok & 1u) != 0);
	hexField(t, "c4fm.raw_term_csd2", r->term_csd2, 20, (r->term_ok & 2u) != 0);
	for (unsigned fn = 0; fn < 8; fn++) {
		char key[16] = "c4fm.raw_dch0";
		key[12] = (char)('0' + fn);
		hexField(t, key, r->dch[fn], YSF_CALLSIGN_LEN, (r->dch_ok & (1u << fn)) != 0);
	}
}
#endif

void debuglink_view_c4fm(uint8_t part, dl_text_t *t)
{
#if CONFIG_C4FM
	dl_text(t, "c4fm.built=1\n");
	if (part == 0) {
		counters(t);
	} else if (part == 1) {
		rawFields(t);
	} else {
		// The capture (c4fm_call.h): samples int16 x C4FM_CAP_SAMPLES at cap.samples, the oldest at head once frozen;
		// frame records c4fm_cap_frame_t x C4FM_CAP_FRAMES at cap.frames, the next written at frame_head % N
		const c4fm_capture_t *c = c4fm_call_capture();
		dl_text(t, "cap.state=%u\ncap.seq=%u\ncap.head=%u\ncap.trigger_ms=%u\ncap.frozen_ms=%u\n", c->state,
			(unsigned)c->seq, c->head, (unsigned)c->trigger_ms, (unsigned)c->frozen_ms);
		dl_text(t, "cap.samples=0x%x\ncap.n=%u\ncap.rate=8000\ncap.frames=0x%x\ncap.frame_n=%u\ncap.frame_size=%u\n"
			"cap.frame_head=%u\n", (unsigned)(uintptr_t)c->samples, (unsigned)C4FM_CAP_SAMPLES,
			(unsigned)(uintptr_t)c->frames, (unsigned)C4FM_CAP_FRAMES, (unsigned)sizeof(c4fm_cap_frame_t),
			c->frame_head);
	}
#else
	(void)part;
	dl_text(t, "c4fm.built=0\n");
#endif
}
