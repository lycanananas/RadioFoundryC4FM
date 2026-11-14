// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM calls (core/c4fm/c4fm_call.c) between two simulated radios: radio A transmits through a modem model that plays
// its byte stream at 9600 bit/s; the bits are modulated to 8 kHz audio and fed to radio B's receiver. Checks the
// stream timing (no underrun), the header and DCH callsigns, the DG-ID filter, and that every voice frame arrives in
// order and unchanged.
#include <stdio.h>
#include <string.h>

#include "c4fm/c4fm_call.h"
#include "c4fm/c4fm_dsp.h"
#include "c4fm/ysf.h"
#include "dmr/ambe/ambeFec.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- the modem model: a byte queue drained at 1200 bytes/s, idle 0x77 when empty while keyed ---

#define QMAX	1024
static uint8_t q[QMAX];
static uint16_t qn;
static bool keyed;
static int underruns;		// idle bytes sent between the first frame and the stop
static bool started_stream;
static uint8_t air[200000];	// everything sent on air
static uint32_t air_n;
static int16_t rx_audio[4096];
static uint16_t rx_n;		// audio waiting for radio B

static bool tx_start(void) { keyed = true; qn = 0; started_stream = false; return true; }
static uint16_t tx_space(void) { return (uint16_t)(QMAX - qn); }
static uint16_t tx_queued(void) { return qn; }
static bool tx_write(const uint8_t *b, uint16_t n)
{
	if (n > QMAX - qn) {
		return false;
	}
	memcpy(q + qn, b, n);
	qn = (uint16_t)(qn + n);
	started_stream = true;
	return true;
}
static void tx_stop(void) { keyed = false; }
static bool tx_on_air(void) { return keyed; }
static bool rx_start(void) { return true; }
static void rx_stop(void) {}
static uint16_t rx_read(int16_t *buf, uint16_t max)
{
	uint16_t n = rx_n < max ? rx_n : max;
	memcpy(buf, rx_audio, n * sizeof(int16_t));
	memmove(rx_audio, rx_audio + n, (rx_n - n) * sizeof(int16_t));
	rx_n = (uint16_t)(rx_n - n);
	return n;
}
static uint16_t no_rx(int16_t *buf, uint16_t max) { (void)buf; (void)max; return 0; }

// Radio A's modem: transmit only (its receiver hears nothing)
static const hal_c4fm_t modemA = {
	.rx_rate_hz = 8000, .rx_start = rx_start, .rx_read = no_rx, .rx_stop = rx_stop,
	.tx_start = tx_start, .tx_space = tx_space, .tx_queued = tx_queued, .tx_write = tx_write, .tx_stop = tx_stop,
	.tx_on_air = tx_on_air,
};

// One 20 ms tick of air: 24 bytes from the queue (or idle)
static void airTick(c4fm_mod_t *mod)
{
	uint8_t chunk[24];
	for (int i = 0; i < 24; i++) {
		if (keyed && qn > 0) {
			chunk[i] = q[0];
			memmove(q, q + 1, --qn);
		} else {
			if (keyed && started_stream) {
				underruns++;
			}
			chunk[i] = keyed ? 0x77 : 0x00;
		}
	}
	if (keyed) {
		memcpy(air + air_n, chunk, 24);
		air_n += 24;
	}
	int16_t pcm[200];
	uint32_t n;
	if (keyed) {
		n = c4fmModulate(mod, chunk, 24 * 8, pcm);
	} else {
		n = 160;
		memset(pcm, 0, sizeof pcm);	// carrier off: silence (a real receiver gives noise)
	}
	if (rx_n + n <= sizeof rx_audio / sizeof rx_audio[0]) {
		memcpy(rx_audio + rx_n, pcm, n * sizeof(int16_t));
		rx_n = (uint16_t)(rx_n + n);
	}
}

// --- voice: A's source counts frames, B's sink checks them ---

static uint32_t src_count;
static bool src_on;

static void makeBits(uint32_t k, uint8_t bits[49])
{
	for (int i = 0; i < 49; i++) {
		bits[i] = (uint8_t)(((k * 2654435761u) >> (i % 32)) & 1u) ^ (uint8_t)((i * k) & 1u);
	}
}

static uint32_t src_ready;	// frames the "encoder" has produced (one per 20 ms while on)
static void src_begin(void *ctx) { (void)ctx; src_on = true; src_count = 0; src_ready = 0; }
static bool src_next(uint8_t f72[9], void *ctx)
{
	(void)ctx;
	if (!src_on || src_count >= src_ready) {
		return false;
	}
	uint8_t bits[49];
	makeBits(src_count++, bits);
	ambeFecEncode(bits, f72);
	return true;
}
static void src_end(void *ctx) { (void)ctx; src_on = false; }
static const dmr_voice_source_t source = { .begin = src_begin, .next = src_next, .end = src_end };

static uint32_t sink_count, sink_bad, sink_calls, sink_ends, sink_first;
static void sink_begin(const dmr_caller_t *c, void *ctx) { (void)c; (void)ctx; sink_calls++; sink_count = 0; }
static void sink_frame(const uint8_t f72[9], void *ctx)
{
	(void)ctx;
	uint8_t bits[49], want[49];
	ambeFecDecode(f72, bits, NULL);
	if (sink_count == 0 && memcmp(bits, ysf_ambe_silence, 49) != 0) {
		// Late entry: find which of the sent frames this is
		for (uint32_t k = 0; k < 300; k++) {
			makeBits(k, want);
			if (memcmp(bits, want, 49) == 0) {
				sink_first = k;
				break;
			}
		}
	}
	makeBits(sink_first + sink_count, want);
	if (memcmp(bits, want, 49) != 0 && memcmp(bits, ysf_ambe_silence, 49) != 0) {
		sink_bad++;
	}
	if (memcmp(bits, ysf_ambe_silence, 49) != 0) {
		sink_count++;
	}
}
static void sink_lost(void *ctx) { (void)ctx; }
static void sink_end(void *ctx) { (void)ctx; sink_ends++; }
static const dmr_voice_sink_t sink = { .begin = sink_begin, .frame = sink_frame, .lost = sink_lost, .end = sink_end };

int main(void)
{
	// Two engines share one static state in c4fm_call.c, so the test runs A and B in turn on one instance: A sends
	// (its stream recorded on air), then the recorded air is played into the same engine as B.
	c4fm_call_init(&modemA);
	c4fm_call_set_voice(NULL, &source);
	c4fm_config_t ca = { .callsign = "sp9abc", .dgid = 0, .radio_id = "K7Q2Z" };
	c4fm_call_configure(&ca);
	CHECK(c4fm_call_start(), "start");

	c4fm_mod_t mod;
	c4fmModInit(&mod, 2000);
	uint32_t t = 1000;
	c4fm_call_ptt(true);
	for (int tick = 0; tick < 100; tick++, t += 20) {	// 2 s of talking
		if (src_on) {
			src_ready++;
		}
		c4fm_call_poll(t);
		airTick(&mod);
	}
	c4fm_call_ptt(false);
	for (int tick = 0; tick < 60 && c4fm_call_transmitting(); tick++, t += 20) {
		c4fm_call_poll(t);
		airTick(&mod);
	}
	CHECK(!c4fm_call_transmitting() && !keyed, "transmission ended and unkeyed");
	CHECK(underruns == 0, "no idle bytes inside the stream (%d)", underruns);
	const c4fm_call_view_t *v = c4fm_call_view();
	printf("A: %u frames (%u voice frames padded), %u voice frames pulled\n", v->tx_frames, v->tx_padded, src_count);
	CHECK(v->tx_frames >= 19 && v->tx_frames <= 22, "about 20 communications frames for 2 s (%u)", v->tx_frames);
	CHECK(src_count >= 95, "voice pulled (%u)", src_count);
	uint32_t sent = src_count;

	// The air: preamble, header, frames, terminator; check the structure directly
	CHECK(air_n >= 120 + 120 * 22, "air length %u", air_n);
	uint32_t pos = 0;
	while (pos < air_n && air[pos] == 0x77) {
		pos++;
	}
	CHECK(pos >= C4FM_PREAMBLE_BYTES, "preamble %u bytes", pos);
	ysf_fich_t f;
	CHECK(ysfSyncErrors(air + pos) == 0 && ysfFichDecode(air + pos, &f, NULL) && f.fi == YSF_FI_HEADER,
	      "header frame after the preamble");
	uint8_t csd1[20];
	CHECK(ysfCsdDecode(air + pos, csd1, NULL) & 1u, "header CSD1");
	CHECK(memcmp(csd1, "*****K7Q2ZSP9ABC    ", 20) == 0, "CSD1 *****radio ID / SP9ABC, as an FT-70D");
	CHECK(f.cm == YSF_CM_GROUP2, "FICH CM 1, as an FT-70D");
	uint32_t fr = pos + 120, comms = 0, fn_ok = 1;
	uint8_t want_fn = 0;
	while (fr + 120 <= air_n && ysfFichDecode(air + fr, &f, NULL) && f.fi == YSF_FI_COMMS) {
		fn_ok &= f.fn == want_fn && f.ft == C4FM_FT && f.dt == YSF_DT_VD2 && f.dgid == 0;
		want_fn = (uint8_t)(want_fn >= C4FM_FT ? 0 : want_fn + 1);
		comms++;
		fr += 120;
	}
	CHECK(fn_ok && comms == v->tx_frames, "FN cycle 0..6 over %u frames", comms);
	CHECK(fr + 120 <= air_n && ysfFichDecode(air + fr, &f, NULL) && f.fi == YSF_FI_TERMINATOR, "terminator last");

	// Radio B: the same engine, receiving the recorded air as audio
	static const hal_c4fm_t modemB = {
		.rx_rate_hz = 8000, .rx_start = rx_start, .rx_read = rx_read, .rx_stop = rx_stop,
		.tx_start = tx_start, .tx_space = tx_space, .tx_queued = tx_queued, .tx_write = tx_write,
		.tx_stop = tx_stop, .tx_on_air = tx_on_air,
	};
	for (int pass = 0; pass < 3; pass++) {
		// pass 0: DG-ID 0 hears it; pass 1: DG-ID 0 configured, late entry (header cut); pass 2: DG-ID 5 ignores it
		c4fm_call_stop();
		c4fm_call_init(&modemB);
		c4fm_call_set_voice(&sink, NULL);
		c4fm_config_t cb = { .callsign = "SP9XYZ", .dgid = pass == 2 ? 5 : 0 };
		c4fm_call_configure(&cb);
		c4fm_call_start();
		if (pass == 0) {
			c4fm_call_capture_ctl(1);	// the bench capture: armed, it freezes 1.5 s after the call starts
		}
		sink_count = sink_bad = sink_calls = sink_ends = sink_first = 0;
		c4fm_mod_t m2;
		c4fmModInit(&m2, 1800);
		uint32_t start = pass == 1 ? pos + 120 * 3 : 0;	// late entry: start at the third communications frame
		bool saw_rx = false, saw_busy = false;
		char src_seen[11] = "";
		for (uint32_t b = start; b < air_n + 240; b += 24, t += 20) {
			uint8_t chunk[24];
			for (int i = 0; i < 24; i++) {
				chunk[i] = b + (uint32_t)i < air_n ? air[b + (uint32_t)i] : 0;
			}
			int16_t pcm[200];
			uint32_t n = c4fmModulate(&m2, chunk, 192, pcm);
			memcpy(rx_audio, pcm, n * sizeof(int16_t));
			rx_n = (uint16_t)n;
			c4fm_call_poll(t);
			const c4fm_call_view_t *w = c4fm_call_view();
			saw_rx |= w->phase == C4FM_PHASE_RX_VOICE;
			saw_busy |= w->phase == C4FM_PHASE_RX_BUSY;
			if (w->src[0] != '\0') {
				strcpy(src_seen, w->src);
			}
		}
		for (int i = 0; i < 30; i++, t += 20) {
			c4fm_call_poll(t);	// hang time passes
		}
		const c4fm_call_view_t *w = c4fm_call_view();
		printf("B pass %d: calls %u, frames %u good / %u bad, src '%s', fich bad %u\n", pass, sink_calls, sink_count,
		       sink_bad, src_seen, w->rx_fich_bad);
		if (pass < 2) {
			CHECK(saw_rx && sink_calls == 1 && sink_ends == 1, "pass %d: one call played and ended", pass);
			CHECK(strcmp(src_seen, "SP9ABC") == 0, "pass %d: caller SP9ABC ('%s')", pass, src_seen);
			CHECK(sink_bad == 0, "pass %d: no corrupted voice frame (%u)", pass, sink_bad);
			CHECK(pass == 1 ? sink_first + sink_count == sent && sink_count > 0 : sink_count == sent,
			      "pass %d: voice frames %u of %u", pass, sink_count, sent);
			CHECK(w->late_entry == (pass == 1), "pass %d: late entry flag", pass);
			// The raw capture (debug link c4fm_raw): A's fields byte for byte, as A's own encoder wrote them
			const c4fm_raw_t *r = c4fm_call_raw();
			uint8_t want[YSF_CALLSIGN_LEN];
			c4fm_callsign_field("SP9ABC", want);
			CHECK(r->dch_ok == 0x7F && memcmp(r->dch[1], want, sizeof want) == 0,
			      "pass %d: raw DCH FN 0-6 captured, FN 1 the caller (0x%02x)", pass, r->dch_ok);
			// The FT-70D's layout (recorded 2026-11-14: "*****H6MMG", "     H6MMG", FN 6 below), our ID and call
			CHECK(memcmp(r->dch[0], "*****K7Q2Z", 10) == 0, "pass %d: raw DCH FN 0 *****radio ID", pass);
			CHECK(memcmp(r->dch[5], "     K7Q2Z", 10) == 0, "pass %d: raw DCH FN 5 radio ID", pass);
			static const uint8_t ft70d_fn6[YSF_CALLSIGN_LEN] = { 0x01, 0x22, 0x61, 0x5F, 0x2B, 0x03, 0x11, 0, 0, 0 };
			CHECK(memcmp(r->dch[6], ft70d_fn6, sizeof ft70d_fn6) == 0, "pass %d: raw DCH FN 6 as the FT-70D's", pass);
			CHECK(memcmp(r->dch[2], "          ", 10) == 0 && memcmp(r->dch[4], "          ", 10) == 0,
			      "pass %d: raw DCH FN 2, 4 blank", pass);
			CHECK(r->fich.cm == YSF_CM_GROUP2, "pass %d: raw FICH CM 1", pass);
			CHECK(strcmp(w->src_id, "K7Q2Z") == 0, "pass %d: caller's radio ID '%s'", pass, w->src_id);
			CHECK((r->term_ok & 1u) && memcmp(r->term_csd1, "*****K7Q2ZSP9ABC    ", 20) == 0,
			      "pass %d: raw terminator CSD1", pass);
			CHECK(r->fich.dt == YSF_DT_VD2 && r->fich.ft == C4FM_FT, "pass %d: raw FICH DT / FT", pass);
			if (pass == 0) {
				const c4fm_capture_t *cp = c4fm_call_capture();
				uint8_t good = 0;
				for (int k = 0; k < C4FM_CAP_FRAMES && k < cp->frame_head; k++) {
					good += (cp->frames[k].flags & 1u) != 0;
				}
				CHECK(cp->state == C4FM_CAP_FROZEN && cp->seq == 1 && cp->frame_head >= 15 && good >= 14,
				      "capture frozen after the call started, %u frames recorded, %u FICH good", cp->frame_head,
				      good);
				CHECK((r->hdr_ok & 3u) == 3u && memcmp(r->hdr_csd1, "*****K7Q2ZSP9ABC    ", 20) == 0,
				      "header CSD1 / CSD2 captured");
				CHECK(w->rx_fich_fec == 0 && w->rx_fich_ok >= 20, "clean channel: %u good FICHs, no bits corrected (%u)",
				      w->rx_fich_ok, w->rx_fich_fec);
			} else {
				CHECK(r->hdr_ok == 0, "late entry: no header captured");
			}
		} else {
			CHECK(!saw_rx && saw_busy && sink_calls == 0, "DG-ID 5 doesn't play a DG-ID 0 call");
		}
		CHECK(w->phase == C4FM_PHASE_IDLE, "pass %d: idle after", pass);
	}

	// Radio IDs: five of 0-9 A-Z, the same for the same seed, different for different chips
	{
		const uint32_t s1[3] = { 0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu }, s2[3] = { 0x12345679u, 0x9ABCDEF0u, 0x0F1E2D3Cu };
		char a[6], b[6], c[6];
		c4fm_radio_id(s1, 3, a);
		c4fm_radio_id(s1, 3, b);
		c4fm_radio_id(s2, 3, c);
		bool charset = strlen(a) == 5;
		for (int i = 0; i < 5; i++) {
			charset &= (a[i] >= '0' && a[i] <= '9') || (a[i] >= 'A' && a[i] <= 'Z');
		}
		CHECK(charset && strcmp(a, b) == 0 && strcmp(a, c) != 0, "radio ID '%s' stable, '%s' for another chip", a, c);
		int same = 0;
		char seen[64][6];
		for (uint32_t k = 0; k < 64; k++) {
			const uint32_t sk[3] = { k, 0x5A5A0000u, 0 };
			c4fm_radio_id(sk, 3, seen[k]);
			for (uint32_t j = 0; j < k; j++) {
				same += strcmp(seen[j], seen[k]) == 0;
			}
		}
		CHECK(same == 0, "64 nearby chips, 64 radio IDs (%d alike)", same);
		// A malformed ID in the configuration sends 00000
		c4fm_config_t bad = { .callsign = "SP9ABC", .radio_id = "ab#12" };
		c4fm_call_configure(&bad);
		CHECK(c4fm_call_config()->radio_id[0] == '\0', "malformed radio ID refused");
	}

	// Wires-X (docs/spec/c4fm.md 5.1): A sends CONNECT to room 12345 as data FR frames; a node's reply (the layout of
	// MMDVM_CM's sendConnectReply) played into B gives the room
	{
		uint8_t cmd[C4FM_WX_DATA_MAX];
		uint16_t cl = c4fm_wx_command(C4FM_WX_CONNECT, 12345, cmd);
		uint8_t sum = 0;
		for (int i = 0; i < 12; i++) {
			sum = (uint8_t)(sum + cmd[i]);
		}
		CHECK(cl == 13 && cmd[0] == 0x00 && cmd[1] == 0x5D && cmd[2] == 0x23 && cmd[3] == 0x5F &&
		      memcmp(cmd + 5, "12345 ", 6) == 0 && cmd[11] == 0x03 && cmd[12] == sum, "CONNECT command bytes");
		CHECK(c4fm_wx_command(C4FM_WX_DX, 0, cmd) == 7 && cmd[2] == 0x71 && cmd[5] == 0x03, "DX command bytes");

		c4fm_call_stop();
		c4fm_call_init(&modemA);
		c4fm_call_set_voice(NULL, NULL);
		c4fm_config_t cw = { .callsign = "SP9ABC", .dgid = 0, .radio_id = "K7Q2Z" };
		c4fm_call_configure(&cw);
		c4fm_call_start();
		air_n = 0;
		qn = 0;
		c4fm_mod_t m3;
		c4fmModInit(&m3, 2000);
		CHECK(c4fm_call_wx(C4FM_WX_CONNECT, 12345), "Wires-X command accepted");
		bool sent = false;
		for (int tick = 0; tick < 200; tick++, t += 20) {
			c4fm_call_poll(t);
			sent |= c4fm_call_transmitting();
			airTick(&m3);
			if (sent && !c4fm_call_transmitting() && !keyed) {
				break;
			}
		}
		CHECK(sent && !keyed && c4fm_call_view()->phase == C4FM_PHASE_IDLE, "command sent, unkeyed, idle");
		CHECK(c4fm_call_wx_state()->sent == C4FM_WX_CONNECT, "sent command noted");
		uint32_t p = 0;
		while (p < air_n && air[p] == 0x77) {
			p++;
		}
		// header, FN 0, FN 1, terminator
		ysf_fich_t ff;
		int frames_ok = 0;
		bool data_ok = false;
		for (int k = 0; k < 4 && p + 120 <= air_n; k++, p += 120) {
			if (ysfSyncErrors(air + p) != 0 || !ysfFichDecode(air + p, &ff, NULL) || ff.dt != YSF_DT_DATA_FR || ff.ft != 1) {
				continue;
			}
			frames_ok++;
			uint8_t d1[20], d2[20];
			if (ff.fi == YSF_FI_COMMS && ff.fn == 1 && ysfCsdDecode(air + p, d1, d2) == 3u) {
				data_ok = d2[1] == 0x5D && d2[2] == 0x23 && memcmp(d2 + 5, "12345 ", 6) == 0 &&
					  d2[11] == 0x03 && memcmp(d1 + 15, "K7Q2Z", 5) == 0;
			}
		}
		CHECK(frames_ok == 4 && data_ok, "on air: 4 data FR frames, FT 1, FN 1 the command and CSD3 the radio ID (%d)",
		      frames_ok);

		// The node's reply into B
		uint8_t rep[100];
		memset(rep, ' ', sizeof rep);
		rep[0] = 0x00;
		memcpy(rep + 1, "\x5D\x41\x5F\x26", 4);
		memcpy(rep + 5, "54321", 5);
		memcpy(rep + 10, "SR5WI     ", 10);
		memcpy(rep + 20, "WARSZAWA      ", 14);
		memcpy(rep + 34, "15", 2);
		memcpy(rep + 36, "12345", 5);
		memcpy(rep + 41, "POLSKA          ", 16);
		memcpy(rep + 57, "000", 3);
		memcpy(rep + 84, "00000", 5);
		rep[89] = 0x03;
		uint8_t rs = 0;
		for (int i = 0; i < 90; i++) {
			rs = (uint8_t)(rs + rep[i]);
		}
		rep[90] = rs;
		static uint8_t tx[120 * 8];
		uint32_t tn = 0;
		memset(tx, 0x77, 120);
		tn = 120;
		uint8_t c1[20], c2[20], c3[20];
		memset(c1, ' ', 20);
		memset(c2, ' ', 20);
		memset(c3, ' ', 20);
		for (int k = 0; k < 6; k++, tn += 120) {	// header, FN 0-3, terminator (FT 3: 91 bytes)
			uint8_t *fr = tx + tn;
			memset(fr, 0, 120);
			ysf_fich_t rf = { .fi = k == 0 ? YSF_FI_HEADER : k == 5 ? YSF_FI_TERMINATOR : YSF_FI_COMMS, .cs = 2,
					  .fn = (uint8_t)(k >= 1 && k <= 4 ? k - 1 : 0), .ft = 3, .dt = YSF_DT_DATA_FR };
			ysfAddSync(fr);
			ysfFichEncode(&rf, fr);
			if (rf.fi != YSF_FI_COMMS || rf.fn == 0) {
				ysfCsdEncode(c1, c2, fr);
			} else if (rf.fn == 1) {
				ysfCsdEncode(c3, rep, fr);
			} else {
				ysfCsdEncode(rep + 20 + (rf.fn - 2) * 40, rep + 40 + (rf.fn - 2) * 40, fr);
			}
		}
		c4fm_call_stop();
		c4fm_call_init(&modemB);
		c4fm_call_configure(&cw);
		c4fm_call_start();
		c4fm_mod_t m4;
		c4fmModInit(&m4, 1800);
		for (uint32_t b = 0; b < tn + 480; b += 24, t += 20) {
			uint8_t chunk[24];
			for (int i = 0; i < 24; i++) {
				chunk[i] = b + (uint32_t)i < tn ? tx[b + (uint32_t)i] : 0x77;
			}
			int16_t pcm[200];
			uint32_t n = c4fmModulate(&m4, chunk, 192, pcm);
			memcpy(rx_audio, pcm, n * sizeof(int16_t));
			rx_n = (uint16_t)n;
			c4fm_call_poll(t);
		}
		const c4fm_wx_t *wx = c4fm_call_wx_state();
		printf("Wires-X reply: node '%s' '%s' %s, room '%s' '%s', connected %d\n", wx->node_call, wx->node_name,
		       wx->node_id, wx->room_id, wx->room_name, wx->connected);
		CHECK(wx->reply_ms != 0 && wx->connected && strcmp(wx->room_id, "12345") == 0 &&
		      strcmp(wx->room_name, "POLSKA") == 0 && strcmp(wx->node_call, "SR5WI") == 0 &&
		      strcmp(wx->node_name, "WARSZAWA") == 0 && strcmp(wx->node_id, "54321") == 0,
		      "the node's connect reply parsed");
		// A reply with a wrong checksum is not taken
		c4fm_wx_t junk = *wx;
		rep[90] ^= 1;
		CHECK(!c4fm_wx_parse(rep, 100, &junk), "a reply with a bad checksum is refused");
	}

	printf("c4fmCallTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
