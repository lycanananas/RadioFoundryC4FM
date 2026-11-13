// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * C4FM voice calls in DN mode (V/D mode 2) on one frequency: what the radio core runs on a C4FM channel.
 *
 * Identity is a callsign (the radio name's callsign), not a numeric ID: there are no contacts and no talk groups. The
 * one selector is the DG-ID (0..99): 0 hears every transmission and sends with DG-ID 0, as Yaesu radios do; another
 * value sends with it and hears only transmissions that carry it.
 *
 * Transmit: preamble, a header frame (CSD: ALL / our callsign), communications frames of five AMBE+2 frames each with
 * the DCH cycling through destination, source, downlink, uplink and data (FT 6: seven frames), then a terminator.
 * The modem is kept between one and two frames ahead of the air; a frame whose voice isn't all there yet when the
 * modem runs low is padded with silence, so the stream never breaks.
 *
 * Receive: discriminator audio -> c4fm_dsp -> frames. A header or the first decodable communications frame (late
 * entry) starts a call; its callsign comes from the header's CSD1 or from the DCH of FN 1. The terminator, a lost
 * signal or C4FM_RX_HANG_MS without a frame ends it. Voice other than DN (VW full rate, data) is shown but not played:
 * the radio's AMBE+2 codec only speaks the half rate frame DN carries.
 *
 * The voice sink and source are the DMR stack's (ambeVoice: 72-bit AMBE+2 frames); the engine converts the 49
 * parameter bits with ambeFec, which is lossless both ways. No RTOS calls: poll() does everything.
 */
#ifndef RFOUNDRY_C4FM_CALL_H
#define RFOUNDRY_C4FM_CALL_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_c4fm.h"
#include "dmr/call/dmr_call.h"	// dmr_voice_sink_t / dmr_voice_source_t: the voice path is shared with DMR
#include "c4fm/c4fm_dsp.h"

#define C4FM_CALLSIGN_LEN	10
#define C4FM_DGID_MAX		99
#define C4FM_RX_HANG_MS		400	// no frame for this long ends a call that had no terminator
#define C4FM_PREAMBLE_BYTES	120	// 100 ms of 0x77 before the header (receivers lock on it)
#define C4FM_FT			6	// DCH cycle of seven frames
#define C4FM_TX_LOW_BYTES	120	// queue below this (100 ms of air): send the next frame, voice or not

typedef enum {
	C4FM_PHASE_IDLE,
	C4FM_PHASE_RX_VOICE,	// a DN call we play
	C4FM_PHASE_RX_BUSY,	// a transmission we don't play (another DG-ID, VW / data mode)
	C4FM_PHASE_TX_VOICE,
	C4FM_PHASE_TX_ENDING,	// terminator queued, playing out
	C4FM_PHASE_TX_DENIED,	// the modem refused to key: release PTT to clear
} c4fm_phase_t;

typedef struct {
	char callsign[C4FM_CALLSIGN_LEN + 1];	// ours, upper case; "" = can't transmit
	uint8_t dgid;				// 0 = all
} c4fm_config_t;

typedef struct {
	uint8_t phase;		// c4fm_phase_t
	uint32_t serial;	// bumped on every change the UI should redraw
	// The call heard (RX) or the last one
	char src[C4FM_CALLSIGN_LEN + 1];	// trailing spaces trimmed, "" until known
	char dst[C4FM_CALLSIGN_LEN + 1];
	uint8_t dgid;
	uint8_t dt;		// YSF_DT_* of the last frame
	bool late_entry;	// no header heard for this call
	// Counters (diagnostics)
	uint32_t rx_frames, rx_fich_bad, rx_calls, rx_vch_errors, rx_ambe;
	uint32_t tx_frames, tx_padded, tx_calls;
	uint32_t dsp_syncs, dsp_losts, dsp_flywheels;
	int16_t rx_gain, rx_center;	// the receiver's last level fit
	bool rx_inverted;
} c4fm_call_view_t;

void c4fm_call_init(const hal_c4fm_t *modem);
void c4fm_call_set_voice(const dmr_voice_sink_t *sink, const dmr_voice_source_t *source);
void c4fm_call_configure(const c4fm_config_t *cfg);
// On a C4FM channel, with the board's RF in C4FM receive: start receiving (false: no modem)
bool c4fm_call_start(void);
// Leaving C4FM: abort any call, stop the modem
void c4fm_call_stop(void);
bool c4fm_call_running(void);
void c4fm_call_ptt(bool down);
void c4fm_call_poll(uint32_t now_ms);
const c4fm_call_view_t *c4fm_call_view(void);
// The transmitter is in use by the engine (TX_VOICE or TX_ENDING)
bool c4fm_call_transmitting(void);

// Exposed for the tests: the callsign as YSF sends it (10 characters, upper case, space padded)
void c4fm_callsign_field(const char *call, uint8_t out[C4FM_CALLSIGN_LEN]);

#endif
