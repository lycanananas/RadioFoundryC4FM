// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM on the HR_C7000 (docs/spec/c4fm.md, modem-hrc7000.md 2.10, 8.5). The chip has no C4FM, but C4FM is DMR's
// 4800 Bd 4FSK with the same root raised cosine and dibit mapping at 1.39 times the deviation, so:
//
//   Transmit: the physical layer mode (WORK_MODE 0x02, continuous) sends 36 bytes per 30 ms slot as given, which the
//   split duplex TX half already uses (hrc7000_bs.c). The C4FM byte stream is cut into those slots back to back; the
//   modulation amplitudes in RF_MOD_BIAS_CTRL (Mod1 phase_reduce, Mod2 sig_reduce) are scaled up for C4FM's
//   deviation while keyed and put back after. An empty queue sends the 0x77 idle pattern.
//   Receive: the chip in FM copies its AF input (the RF chip's discriminator) to the RX RAM at 8 kHz
//   (platform/hrc7000/fm_rx_audio.c); the core recovers the symbols. The band-pass of the FM receive chain is off.
//
// The slot clock and the PA keying are the split duplex TX half's: a boundary at the first PHY_RDY or timeslot
// interrupt in a slot, the RF chip and PA keyed once at the first RF TX interrupt and held to the end.
#ifndef HRC7000_C4FM_H
#define HRC7000_C4FM_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc7000_dmr.h"

#define HRC7000_C4FM_Q		1024u	// bytes queued (850 ms of air)
#define HRC7000_C4FM_SLOT	36u	// bytes per physical layer slot
#define HRC7000_C4FM_LEVEL_DEF	139u	// modulation amplitude in percent of DMR's: +-2700 / +-1944 Hz

typedef struct {
	uint32_t slots;		// slot boundaries while running
	uint32_t sent;		// slots filled from the queue
	uint32_t idle;		// slots filled (partly) with idle: the queue ran dry while keyed
	uint32_t phy_ints, ts_ints, rftx_ints;
	uint32_t stalls;	// no slot interrupt for 200 ms: PA dropped
	uint32_t keyups;
	uint32_t mod_bias_dmr, mod_bias_c4fm;	// RF_MOD_BIAS_CTRL before and while keyed (bench)
} hrc7000_c4fm_stats_t;

typedef struct hrc7000_c4fm {
	uint8_t q[HRC7000_C4FM_Q];
	volatile uint16_t head, tail;	// free running byte counters: the task moves head, the ISR tail
	volatile bool keyed;		// the stream is on (until tx_stop)
	bool pa;			// RF chip and PA keyed
	bool timing;
	uint32_t t_tick;
	uint8_t level_pct;		// modulation amplitude, percent of DMR's (HRC7000_C4FM_LEVEL_DEF)
	uint32_t saved_bias;
	hrc7000_c4fm_stats_t st;
} hrc7000_c4fm_t;

// Task side. tx_start takes the chip from DMR (hrc7000_dmr_enter done, the engine not running) into the physical
// layer and keys at the next slot; false while one runs. tx_stop: PA off, the chip back to DMR (the caller sets up
// receive again)
bool hrc7000_c4fm_tx_start(hrc7000_c4fm_t *c, hrc7000_dmr_t *e);
void hrc7000_c4fm_tx_stop(hrc7000_dmr_t *e);
uint16_t hrc7000_c4fm_space(const hrc7000_c4fm_t *c);
uint16_t hrc7000_c4fm_queued(const hrc7000_c4fm_t *c);
bool hrc7000_c4fm_write(hrc7000_c4fm_t *c, const uint8_t *b, uint16_t n);
// Stall watchdog: call every 10 ms or so while transmitting
void hrc7000_c4fm_poll(hrc7000_dmr_t *e);

// ISR side (the DMR engine's vectors hand over while e->c4fm is set)
void hrc7000_c4fm_isr_sys(hrc7000_dmr_t *e, uint32_t list);
void hrc7000_c4fm_isr_timeslot(hrc7000_dmr_t *e);
void hrc7000_c4fm_isr_rftx(hrc7000_dmr_t *e);

// RF_MOD_BIAS_CTRL with the two modulation amplitudes (b15:8, b7:0) scaled by pct / 100, each capped at 255
uint32_t hrc7000_c4fm_scale_bias(uint32_t bias, uint16_t pct);

#endif
