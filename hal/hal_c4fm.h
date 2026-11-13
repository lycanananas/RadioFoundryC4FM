// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM (Yaesu System Fusion) modem as a board service (docs/spec/c4fm.md). The core builds every frame and recovers
// symbols itself (core/c4fm); the board only moves bits to a 4800 Bd 4FSK transmitter and discriminator audio back.
//
// Transmit: a byte stream at 9600 bit/s, MSB first, two bits per symbol with the DMR dibit mapping (01 +3, 00 +1,
// 10 -1, 11 -3) at the C4FM deviation (+-2700 Hz outer). tx_start keys the transmitter and starts the stream; bytes
// queued with tx_write go out back to back; when the queue runs dry the board sends idle (the preamble pattern 0x77)
// until tx_stop. The radio core has the channel's TX frequency and power set before tx_start.
//
// Receive: the discriminator (flat, not de-emphasized where the board can) as signed 16-bit samples at rx_rate_hz,
// read from the radio task every poll; the board buffers at least 100 ms. Unsquelched: the core finds the signal.
#ifndef HAL_C4FM_H
#define HAL_C4FM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// Receive
	uint32_t rx_rate_hz;
	bool (*rx_start)(void);
	uint16_t (*rx_read)(int16_t *buf, uint16_t max);
	void (*rx_stop)(void);

	// Transmit
	bool (*tx_start)(void);
	// Bytes the queue takes now / holds now (not yet on air)
	uint16_t (*tx_space)(void);
	uint16_t (*tx_queued)(void);
	// Queue n bytes; false (nothing queued) if they don't fit
	bool (*tx_write)(const uint8_t *bytes, uint16_t n);
	// Unkey (after the queue played out, or at once) and back to receive
	void (*tx_stop)(void);
	// The transmitter is keyed and sending
	bool (*tx_on_air)(void);

	// Optional, bench: the transmit deviation in percent of the board's C4FM default (100); NULL when fixed
	void (*set_tx_level)(uint8_t percent);
} hal_c4fm_t;

#endif
