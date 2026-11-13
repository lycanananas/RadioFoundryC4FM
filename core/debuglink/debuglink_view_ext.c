// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dmr_ext view section (docs/spec/debuglink.md 3.7): DMR emergency (core/dmr/dmr_emergency.h), the ETSI short
// data counters and settings (core/dmr/dmrDataService.h) and the hotspot's CW ID (core/radio/radio_hotspot.h). Apart
// from dmr_data so that section keeps fitting one reply.
#if CONFIG_C4FM
#include "c4fm/c4fm_call.h"
#endif
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#if CONFIG_DMR_DATA_CORE
#include "dmr/dmr_data_api.h"
#endif
#if CONFIG_DMR_EMERGENCY
#include "dmr/dmr_emergency.h"
#endif
#if CONFIG_HOTSPOT
#include "radio/radio_hotspot.h"
#endif

#if CONFIG_DMR_EMERGENCY
static const char *em_state_key(uint8_t s)
{
	static const char *const names[] = { "idle", "alarm", "hot_mic", "active", "failed" };
	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *em_mode_key(uint8_t m)
{
	static const char *const names[] = { "alarm", "alarm_call", "call" };
	return m < sizeof(names) / sizeof(names[0]) ? names[m] : "?";
}
#endif

void debuglink_view_dmr_ext(dl_text_t *t)
{
#if CONFIG_C4FM
	// C4FM (docs/spec/c4fm.md): the engine's and the receiver's counters, then the board driver's
	const c4fm_call_view_t *c = c4fm_call_view();
	dl_text(t, "c4fm.built=1\nc4fm.phase=%u\nc4fm.src=%s\nc4fm.dst=%s\nc4fm.dgid=%u\nc4fm.dt=%u\nc4fm.late=%u\n",
		c->phase, c->src, c->dst, c->dgid, c->dt, c->late_entry);
	dl_text(t, "c4fm.rx_frames=%u\nc4fm.rx_fich_bad=%u\nc4fm.rx_calls=%u\nc4fm.rx_vch_errors=%u\nc4fm.rx_ambe=%u\n",
		(unsigned)c->rx_frames, (unsigned)c->rx_fich_bad, (unsigned)c->rx_calls, (unsigned)c->rx_vch_errors,
		(unsigned)c->rx_ambe);
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
#endif
#if CONFIG_DMR_DATA_CORE
	dmrDataStats_t s;
	dmrDataSettings_t set;

	dmrDataServiceGetStats(&s);
	dmr_data_get_settings(&set);
	dl_text(t, "data.sms_format=%s\ndata.radio_check_etsi=%u\ndata.tms_built=%u\n",
		set.smsFormat == DMR_SMS_FORMAT_ETSI ? "etsi" : set.smsFormat == DMR_SMS_FORMAT_ETSI_IP ? "etsi_ip" : "tms",
		set.radioCheckEtsi, (unsigned)CONFIG_DMR_TMS);
	dl_text(t, "data.etsi_sms_received=%u\ndata.etsi_sms_sent=%u\ndata.etsi_responses=%u\n",
		(unsigned)s.etsiSmsReceived, (unsigned)s.etsiSmsSent, (unsigned)s.etsiResponsesReceived);
	dl_text(t, "data.ip_text_received=%u\ndata.ip_text_sent=%u\ndata.pt_active=%u\ndata.pt_yielded=%u\n",
		(unsigned)s.ipTextReceived, (unsigned)s.ipTextSent, (unsigned)dmrDataServicePassthrough(),
		(unsigned)s.ptYielded);
	dl_text(t, "data.emergency_alarms_heard=%u\ndata.emergency_alarms_received=%u\ndata.emergency_alarms_sent=%u\n",
		(unsigned)s.emergencyAlarmsHeard, (unsigned)s.emergencyAlarmsReceived, (unsigned)s.emergencyAlarmsSent);
#endif
#if CONFIG_DMR_EMERGENCY
	dmr_emergency_settings_t es;
	dmr_emergency_alert_t a;
	dmr_emergency_stats_t st;
	bool active = dmr_emergency_alert(&a);

	dmr_emergency_get_settings(&es);
	dmr_emergency_get_stats(&st);
	dl_text(t, "em.built=1\nem.state=%s\nem.serial=%u\n", em_state_key(dmr_emergency_state()),
		(unsigned)dmr_emergency_serial());
	dl_text(t, "em.mode=%s\nem.dst=%u\nem.dst_private=%u\nem.tries=%u\nem.hot_mic_s=%u\nem.etsi_fid=%u\nem.rx_tone=%u\n",
		em_mode_key(es.mode), (unsigned)es.dst, es.dst_private, es.alarm_tries, es.hot_mic_s, es.etsi_fid,
		es.rx_tone);
	dl_text(t, "em.alert=%u\nem.alert_kind=%s\nem.alert_src=%u\nem.alert_dst=%u\nem.alert_group=%u\n"
		"em.alert_pending=%u\nem.alert_serial=%u\n", active,
		a.kind == DMR_EMERGENCY_ALERT_ALARM ? "alarm" : a.kind == DMR_EMERGENCY_ALERT_CALL ? "call" : "none",
		(unsigned)a.src, (unsigned)a.dst, a.group, a.pending, (unsigned)a.serial);
	dl_text(t, "em.started=%u\nem.failed=%u\nem.alarms_started=%u\nem.hot_mic_keyed=%u\nem.alerts=%u\n"
		"em.alerts_alarm=%u\nem.alerts_call=%u\nem.tones=%u\n", (unsigned)st.started, (unsigned)st.failed,
		(unsigned)st.alarms_started, (unsigned)st.hot_mic_keyed, (unsigned)st.alerts, (unsigned)st.alerts_alarm,
		(unsigned)st.alerts_call, (unsigned)st.tones);
#else
	dl_text(t, "em.built=0\n");
#endif
#if CONFIG_HOTSPOT
	radio_hs_cw_stats_t cw;

	radio_hs_cw_get_stats(&cw);
	dl_text(t, "hs.cw_requests=%u\nhs.cw_sent=%u\nhs.cw_refused=%u\nhs.cw_last_ms=%u\nhs.cw_wpm=%u\nhs.cw_active=%u\n",
		(unsigned)cw.requests, (unsigned)cw.sent, (unsigned)cw.refused, (unsigned)cw.last_ms, cw.wpm, cw.active);
#endif
}
