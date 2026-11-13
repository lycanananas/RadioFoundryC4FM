// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM on the HR_C7000 (hrc7000_c4fm.h)
#include <stddef.h>
#include <string.h>
#include "hrc7000_c4fm.h"

#if defined(CONFIG_C4FM) && CONFIG_C4FM	// the whole file

#define WORK_MODE_PHY_CONT	0x02u	// physical layer, continuous mode (DS 9.5.3)
#define L2_PHY_TX_MASTER	0x82u	// txen, tx_master_mode: own TX slot timing
#define L2_PASSIVE		0xC0u
#define TXRX_SEND		0x80u	// txnextsloten
#define TXRX_NONE		0x00u
#define TXRX_SYNC_RELEASE	0x800000u
#define TXRX_TX_SYNC_RELEASE	0x400000u
#define SYS_PHY_RDY		(1u << 12)
#define RAM_HALF0		0x00u
#define RAM_HALF1		0x24u
#define SLOT_US			30000u
#define STALL_US		200000u
#define IDLE_BYTE		0x77u	// +3 -3 +3 -3: the preamble pattern

static inline void w(hrc7000_dmr_t *e, uint32_t off, uint32_t v) { hrc7000_wr(e->dev, off, v); }
static inline uint32_t r(hrc7000_dmr_t *e, uint32_t off) { return hrc7000_rd(e->dev, off); }
static inline uint32_t now(hrc7000_dmr_t *e) { return e->ops->now_us(e->ops->ctx); }
static void lock(hrc7000_dmr_t *e) { if (e->ops->lock != NULL) e->ops->lock(e->ops->ctx); }
static void unlock(hrc7000_dmr_t *e) { if (e->ops->unlock != NULL) e->ops->unlock(e->ops->ctx); }

uint32_t hrc7000_c4fm_scale_bias(uint32_t bias, uint16_t pct)
{
	uint32_t m2 = ((bias >> 8) & 0xFFu) * pct / 100u, m1 = (bias & 0xFFu) * pct / 100u;
	m2 = m2 > 255u ? 255u : m2;
	m1 = m1 > 255u ? 255u : m1;
	return (bias & 0xFFFF0000u) | (m2 << 8) | m1;
}

static void pa_off(hrc7000_dmr_t *e, hrc7000_c4fm_t *c)
{
	if (c->pa) {
		e->ops->rf(e->ops->ctx, false);
		c->pa = false;
		e->rf_tx = false;
	}
}

bool hrc7000_c4fm_tx_start(hrc7000_c4fm_t *c, hrc7000_dmr_t *e)
{
	lock(e);
	bool ok = e->c4fm == NULL && !e->running;
	if (ok) {
		uint8_t level = c->level_pct != 0 ? c->level_pct : HRC7000_C4FM_LEVEL_DEF;
		hrc7000_c4fm_stats_t st = c->st;
		memset(c, 0, offsetof(hrc7000_c4fm_t, level_pct));	// the queue and state, not the level or the counters
		c->level_pct = level;
		c->st = st;
		c->st.keyups++;
		// The board's tx() wrote the DMR modulation balance for the TX frequency: scale its amplitudes for C4FM
		c->saved_bias = r(e, C7_RF_MOD_BIAS);
		uint32_t bias = hrc7000_c4fm_scale_bias(c->saved_bias, level);
		w(e, C7_RF_MOD_BIAS, bias);
		c->st.mod_bias_dmr = c->saved_bias;
		c->st.mod_bias_c4fm = bias;
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		w(e, C7_LAYER2_SLOTON, 0);
		// Through FM, as every DMR entry: resets the state machine. Then the physical layer, own TX timing
		w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
		w(e, C7_WORK_MODE, WORK_MODE_PHY_CONT);
		w(e, C7_LAYER2_CONTROL, L2_PHY_TX_MASTER);
		w(e, C7_AUDIO_BUFFER_CLR, 0x03);
		e->timing = false;
		e->t_tick = now(e);
		c->keyed = true;
		e->c4fm = c;
	}
	unlock(e);
	return ok;
}

void hrc7000_c4fm_tx_stop(hrc7000_dmr_t *e)
{
	lock(e);
	hrc7000_c4fm_t *c = e->c4fm;
	if (c != NULL) {
		c->keyed = false;
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		pa_off(e, c);
		w(e, C7_RF_MOD_BIAS, c->saved_bias);
		w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
		w(e, C7_WORK_MODE, C7_WORK_MODE_DMR);
		w(e, C7_LAYER2_CONTROL, L2_PASSIVE);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE | TXRX_TX_SYNC_RELEASE);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		e->c4fm = NULL;
		e->timing = false;
	}
	unlock(e);
}

uint16_t hrc7000_c4fm_queued(const hrc7000_c4fm_t *c)
{
	return (uint16_t)(c->head - c->tail);
}

uint16_t hrc7000_c4fm_space(const hrc7000_c4fm_t *c)
{
	return (uint16_t)(HRC7000_C4FM_Q - hrc7000_c4fm_queued(c));
}

bool hrc7000_c4fm_write(hrc7000_c4fm_t *c, const uint8_t *b, uint16_t n)
{
	if (n > hrc7000_c4fm_space(c)) {
		return false;
	}
	uint16_t h = c->head;
	for (uint16_t i = 0; i < n; i++) {
		c->q[(uint16_t)(h + i) % HRC7000_C4FM_Q] = b[i];
	}
	__asm__ volatile("" ::: "memory");	// the bytes before the head that publishes them
	c->head = (uint16_t)(h + n);
	return true;
}

void hrc7000_c4fm_poll(hrc7000_dmr_t *e)
{
	lock(e);
	hrc7000_c4fm_t *c = e->c4fm;
	if (c != NULL && (c->keyed || c->pa) && (uint32_t)(now(e) - e->t_tick) > STALL_US) {
		// The slot clock never started or stopped: never leave the PA up without it
		c->st.stalls++;
		e->t_tick = now(e);
		pa_off(e, c);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	}
	unlock(e);
}

// --- ISR side ---

// The slot after the boundary that just passed: the next 36 bytes of the stream (idle where it ran dry)
static void boundary(hrc7000_dmr_t *e, hrc7000_c4fm_t *c)
{
	uint32_t t = now(e);
	if (e->timing && t - e->t_tick < SLOT_US / 2u) {
		return;		// a second interrupt in the same slot
	}
	e->timing = true;
	e->t_tick = t;
	e->tick++;
	c->st.slots++;
	if (!c->keyed) {
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		pa_off(e, c);
		return;
	}
	uint16_t avail = (uint16_t)(c->head - c->tail), tail = c->tail;
	bool short_slot = avail < HRC7000_C4FM_SLOT;
	volatile uint8_t *ram = e->dev->buf;
	for (uint8_t i = 0; i < HRC7000_C4FM_SLOT; i++) {
		uint8_t b = i < avail ? c->q[(uint16_t)(tail + i) % HRC7000_C4FM_Q] : IDLE_BYTE;
		ram[C7_TX_RAM + RAM_HALF0 + i] = b;
		ram[C7_TX_RAM + RAM_HALF1 + i] = b;
	}
	c->tail = (uint16_t)(tail + (short_slot ? avail : HRC7000_C4FM_SLOT));
	if (short_slot) {
		c->st.idle++;
	} else {
		c->st.sent++;
	}
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_SEND);
}

void hrc7000_c4fm_isr_sys(hrc7000_dmr_t *e, uint32_t list)
{
	if ((list & SYS_PHY_RDY) != 0) {
		e->c4fm->st.phy_ints++;
		boundary(e, e->c4fm);
	}
}

void hrc7000_c4fm_isr_timeslot(hrc7000_dmr_t *e)
{
	e->c4fm->st.ts_ints++;
	boundary(e, e->c4fm);
}

void hrc7000_c4fm_isr_rftx(hrc7000_dmr_t *e)
{
	hrc7000_c4fm_t *c = e->c4fm;
	c->st.rftx_ints++;
	if (!c->keyed || c->pa) {
		return;		// keyed once for the whole transmission
	}
	e->ops->rf(e->ops->ctx, true);
	e->ops->pa_arm(e->ops->ctx);
	c->pa = true;
	e->rf_tx = true;
	e->t_rf_tx = now(e);
}

#endif	// CONFIG_C4FM
