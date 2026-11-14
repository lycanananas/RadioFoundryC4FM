// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mode menus' service side (ui_modes.c, CONFIG_C4FM builds): FM's channel rows, the C4FM stations heard
#ifndef UI_MODES_H
#define UI_MODES_H

#include <stdbool.h>
#include <stdint.h>
#include "fmt.h"
#include "radio/radio.h"

#define UI_C4FM_HEARD_MAX	10

typedef struct {
	char call[11];		// callsign as sent
	char id[6];		// radio ID, "" unknown
	uint8_t dgid;
	bool busy;		// heard but not played (another DG-ID, not DN)
	uint32_t ms;		// hal_millis() when last on air
} ui_c4fm_heard_t;

// Every UI tick with the view: notes the C4FM station on air
void ui_modes_poll(const radio_view_t *v);
uint8_t ui_c4fm_heard_count(void);
const ui_c4fm_heard_t *ui_c4fm_heard(uint8_t i);	// 0 = the latest
// A tones_picker index as text: "Off", "88.5 Hz", "D023N"
void ui_modes_tone_text(int32_t index, sbuf_t *b);

#endif
