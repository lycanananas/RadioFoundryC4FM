// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM DN voice calls (c4fm_call.h)
#include "c4fm_call.h"

#include <stddef.h>
#include <string.h>

#include "c4fm/ysf.h"
#include "dmr/ambe/ambeFec.h"

static const hal_c4fm_t *modem;
static const dmr_voice_sink_t *sink;
static const dmr_voice_source_t *source;
static c4fm_config_t cfg;
static c4fm_call_view_t view;
static c4fm_demod_t demod;
static bool running;
static bool ptt;
static uint32_t now;

// Receive
static bool in_call;		// the sink has a call open
static uint32_t last_frame_ms;
static uint8_t fn_next;		// FN expected next (a frame whose FICH failed is taken as this one)

// Transmit
static uint8_t tx_fn;
static uint8_t tx_ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS];
static uint8_t tx_have;		// voice frames pulled for the next communications frame
static bool tx_term;		// terminator queued

static void changed(void)
{
	view.serial++;
}

void c4fm_callsign_field(const char *call, uint8_t out[C4FM_CALLSIGN_LEN])
{
	unsigned i = 0;
	for (; call != NULL && call[i] != '\0' && i < C4FM_CALLSIGN_LEN; i++) {
		char c = call[i];
		out[i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
	}
	for (; i < C4FM_CALLSIGN_LEN; i++) {
		out[i] = ' ';
	}
}

// A received 10-byte field to text: printable characters only, trailing spaces trimmed
static void fieldText(const uint8_t *f, char out[C4FM_CALLSIGN_LEN + 1])
{
	int n = 0;
	for (int i = 0; i < C4FM_CALLSIGN_LEN; i++) {
		char c = (char)f[i];
		out[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
	}
	for (n = C4FM_CALLSIGN_LEN; n > 0 && out[n - 1] == ' '; n--) {
	}
	out[n] = '\0';
}

// --- receive ---

static void callEnd(void)
{
	if (in_call && sink != NULL && sink->end != NULL) {
		sink->end(sink->ctx);
	}
	in_call = false;
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		view.phase = C4FM_PHASE_IDLE;
		changed();
	}
}

static void callBegin(bool play, bool late)
{
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		return;
	}
	view.src[0] = view.dst[0] = '\0';
	view.late_entry = late;
	view.rx_calls++;
	view.phase = play ? C4FM_PHASE_RX_VOICE : C4FM_PHASE_RX_BUSY;
	if (play && sink != NULL) {
		if (sink->begin != NULL) {
			sink->begin(NULL, sink->ctx);
		}
		in_call = true;
	}
	changed();
}

static void playAmbe(const uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS])
{
	if (!in_call || sink == NULL) {
		return;
	}
	for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
		uint8_t f72[AMBE_FRAME_BYTES];
		ambeFecEncode(ambe[j], f72);
		sink->frame(f72, sink->ctx);
		view.rx_ambe++;
	}
}

static void onFrame(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, void *ctx)
{
	(void)ctx;
	ysf_fich_t f;
	bool fich = ysfFichDecode(frame, &f, NULL);
	view.rx_gain = info->gain;
	view.rx_center = info->center;
	view.rx_inverted = info->inverted;
	if (!fich) {
		view.rx_fich_bad++;
		if (view.phase != C4FM_PHASE_RX_VOICE || info->flywheel) {
			return;		// nothing to go on: no call open, or a frame without a sync (likely the end)
		}
		// In a call: take it as the communications frame due (its voice has FEC of its own)
		memset(&f, 0, sizeof f);
		f.fi = YSF_FI_COMMS;
		f.dt = YSF_DT_VD2;
		f.fn = fn_next;
		f.ft = C4FM_FT;
		f.dgid = view.dgid;
	}
	last_frame_ms = now;
	view.rx_frames++;
	fn_next = (uint8_t)(f.fn >= f.ft ? 0 : f.fn + 1);

	bool ours = cfg.dgid == 0 || f.dgid == cfg.dgid;
	bool dn = f.dt == YSF_DT_VD2;

	switch (f.fi) {
	case YSF_FI_HEADER: {
		callEnd();	// a header always starts a new call
		view.dgid = f.dgid;
		view.dt = f.dt;
		callBegin(ours && dn, false);
		uint8_t csd1[20];
		if ((ysfCsdDecode(frame, csd1, NULL) & 1u) != 0) {
			fieldText(csd1, view.dst);
			fieldText(csd1 + 10, view.src);
			changed();
		}
		break;
	}
	case YSF_FI_COMMS: {
		if (view.phase != C4FM_PHASE_RX_VOICE && view.phase != C4FM_PHASE_RX_BUSY) {
			view.dgid = f.dgid;
			view.dt = f.dt;
			callBegin(ours && dn, true);
		}
		if (f.dt != view.dt) {
			view.dt = f.dt;
			changed();
		}
		if (!dn) {
			break;
		}
		uint8_t dch[YSF_CALLSIGN_LEN], ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS], errs[YSF_VCH_PER_FRAME];
		bool dch_ok = ysfVd2Decode(frame, dch, ambe, errs);
		for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
			view.rx_vch_errors += errs[j];
		}
		if (dch_ok && (f.fn == 0 || f.fn == 1)) {
			char *dst = f.fn == 1 ? view.src : view.dst;
			char text[C4FM_CALLSIGN_LEN + 1];
			fieldText(dch, text);
			if (strcmp(text, dst) != 0) {
				memcpy(dst, text, sizeof text);
				changed();
			}
		}
		playAmbe((const uint8_t (*)[YSF_AMBE_BITS])ambe);
		break;
	}
	case YSF_FI_TERMINATOR:
		callEnd();
		break;
	default:
		break;
	}
}

static void onLost(void *ctx)
{
	(void)ctx;
	callEnd();
}

static void rxService(void)
{
	if (modem == NULL || modem->rx_read == NULL) {
		return;
	}
	int16_t buf[128];
	uint16_t n;
	while ((n = modem->rx_read(buf, sizeof buf / sizeof buf[0])) > 0) {
		c4fmDemodFeed(&demod, buf, n);
	}
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		if (now - last_frame_ms > C4FM_RX_HANG_MS) {
			callEnd();
		}
	}
	view.dsp_syncs = demod.syncs;
	view.dsp_losts = demod.losts;
	view.dsp_flywheels = demod.flywheels;
}

// --- transmit ---

static void fichFor(uint8_t fi, uint8_t fn, ysf_fich_t *f)
{
	memset(f, 0, sizeof *f);
	f->fi = fi;
	f->cs = 2;
	f->cm = YSF_CM_GROUP1;
	f->fn = fn;
	f->ft = C4FM_FT;
	f->mr = YSF_MR_DIRECT;
	f->dt = YSF_DT_VD2;
	f->dgid = cfg.dgid;
}

static void csdFrame(uint8_t fi, uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t csd1[20], csd2[20];
	c4fm_callsign_field("ALL", csd1);
	c4fm_callsign_field(cfg.callsign, csd1 + 10);
	memset(csd2, ' ', sizeof csd2);
	ysf_fich_t f;
	fichFor(fi, 0, &f);
	memset(frame, 0, YSF_FRAME_BYTES);
	ysfAddSync(frame);
	ysfFichEncode(&f, frame);
	ysfCsdEncode(csd1, csd2, frame);
}

// DCH content by FN: destination, source, downlink, uplink, two remarks (blank), then data (none: zeros)
static void dchFor(uint8_t fn, uint8_t dch[YSF_CALLSIGN_LEN])
{
	switch (fn) {
	case 0:
		c4fm_callsign_field("ALL", dch);
		break;
	case 1:
		c4fm_callsign_field(cfg.callsign, dch);
		break;
	case 6:
	case 7:
		memset(dch, 0, YSF_CALLSIGN_LEN);
		break;
	default:
		memset(dch, ' ', YSF_CALLSIGN_LEN);
		break;
	}
}

static bool txWrite(const uint8_t *b, uint16_t n)
{
	return modem->tx_write(b, n);
}

static void pullVoice(void)
{
	while (tx_have < YSF_VCH_PER_FRAME && source != NULL) {
		uint8_t f72[AMBE_FRAME_BYTES];
		if (!source->next(f72, source->ctx)) {
			break;
		}
		ambeFecDecode(f72, tx_ambe[tx_have], NULL);
		tx_have++;
	}
}

static void commsFrame(void)
{
	for (; tx_have < YSF_VCH_PER_FRAME; tx_have++) {
		memcpy(tx_ambe[tx_have], ysf_ambe_silence, YSF_AMBE_BITS);
		view.tx_padded++;
	}
	uint8_t frame[YSF_FRAME_BYTES], dch[YSF_CALLSIGN_LEN];
	ysf_fich_t f;
	fichFor(YSF_FI_COMMS, tx_fn, &f);
	memset(frame, 0, sizeof frame);
	ysfAddSync(frame);
	ysfFichEncode(&f, frame);
	dchFor(tx_fn, dch);
	ysfVd2Encode(dch, (const uint8_t (*)[YSF_AMBE_BITS])tx_ambe, frame);
	txWrite(frame, sizeof frame);
	view.tx_frames++;
	tx_have = 0;
	tx_fn = (uint8_t)(tx_fn >= C4FM_FT ? 0 : tx_fn + 1);
}

static void txBegin(void)
{
	callEnd();
	if (modem == NULL || cfg.callsign[0] == '\0' || !modem->tx_start()) {
		view.phase = C4FM_PHASE_TX_DENIED;
		changed();
		return;
	}
	uint8_t pre[C4FM_PREAMBLE_BYTES];
	memset(pre, 0x77, sizeof pre);
	txWrite(pre, sizeof pre);
	uint8_t frame[YSF_FRAME_BYTES];
	csdFrame(YSF_FI_HEADER, frame);
	txWrite(frame, sizeof frame);
	tx_fn = 0;
	tx_have = 0;
	tx_term = false;
	if (source != NULL && source->begin != NULL) {
		source->begin(source->ctx);
	}
	view.tx_calls++;
	view.phase = C4FM_PHASE_TX_VOICE;
	changed();
}

static void txService(void)
{
	if (view.phase == C4FM_PHASE_TX_VOICE) {
		pullVoice();
		// A whole frame of voice, or the air about to run out: send. One frame per poll at most
		if (modem->tx_space() >= YSF_FRAME_BYTES &&
		    (tx_have == YSF_VCH_PER_FRAME || modem->tx_queued() < C4FM_TX_LOW_BYTES)) {
			commsFrame();
		}
		if (!ptt) {
			if (source != NULL && source->end != NULL) {
				source->end(source->ctx);
			}
			view.phase = C4FM_PHASE_TX_ENDING;
			changed();
		}
		return;
	}
	if (view.phase == C4FM_PHASE_TX_ENDING) {
		if (!tx_term) {
			if (tx_have > 0 && modem->tx_space() >= YSF_FRAME_BYTES) {
				commsFrame();	// what voice is left, padded
			}
			if (tx_have == 0 && modem->tx_space() >= YSF_FRAME_BYTES) {
				uint8_t frame[YSF_FRAME_BYTES];
				csdFrame(YSF_FI_TERMINATOR, frame);
				txWrite(frame, sizeof frame);
				tx_term = true;
			}
			return;
		}
		if (modem->tx_queued() == 0) {
			modem->tx_stop();
			c4fmDemodReset(&demod);
			view.phase = C4FM_PHASE_IDLE;
			changed();
		}
	}
}

// --- API ---

void c4fm_call_init(const hal_c4fm_t *m)
{
	modem = m;
	memset(&view, 0, sizeof view);
	c4fmDemodInit(&demod, onFrame, onLost, NULL);
	running = false;
	ptt = false;
	in_call = false;
}

void c4fm_call_set_voice(const dmr_voice_sink_t *s, const dmr_voice_source_t *src)
{
	sink = s;
	source = src;
}

void c4fm_call_configure(const c4fm_config_t *c)
{
	uint8_t field[C4FM_CALLSIGN_LEN];
	cfg.dgid = c->dgid > C4FM_DGID_MAX ? 0 : c->dgid;
	c4fm_callsign_field(c->callsign, field);
	fieldText(field, cfg.callsign);
}

bool c4fm_call_start(void)
{
	if (running) {
		return true;
	}
	if (modem == NULL || modem->rx_start == NULL || !modem->rx_start()) {
		return false;
	}
	c4fmDemodReset(&demod);
	running = true;
	return true;
}

void c4fm_call_stop(void)
{
	if (!running) {
		return;
	}
	callEnd();
	if (c4fm_call_transmitting()) {
		if (source != NULL && source->end != NULL) {
			source->end(source->ctx);
		}
		modem->tx_stop();
	}
	if (modem->rx_stop != NULL) {
		modem->rx_stop();
	}
	view.phase = C4FM_PHASE_IDLE;
	ptt = false;
	running = false;
	changed();
}

bool c4fm_call_running(void)
{
	return running;
}

void c4fm_call_ptt(bool down)
{
	ptt = down;
}

bool c4fm_call_transmitting(void)
{
	return view.phase == C4FM_PHASE_TX_VOICE || view.phase == C4FM_PHASE_TX_ENDING;
}

void c4fm_call_poll(uint32_t now_ms)
{
	now = now_ms;
	if (!running) {
		return;
	}
	if (ptt && (view.phase == C4FM_PHASE_IDLE || view.phase == C4FM_PHASE_RX_VOICE ||
		    view.phase == C4FM_PHASE_RX_BUSY)) {
		txBegin();
	}
	if (!ptt && view.phase == C4FM_PHASE_TX_DENIED) {
		view.phase = C4FM_PHASE_IDLE;
		changed();
	}
	if (c4fm_call_transmitting()) {
		txService();
	} else {
		rxService();
	}
}

const c4fm_call_view_t *c4fm_call_view(void)
{
	return &view;
}
