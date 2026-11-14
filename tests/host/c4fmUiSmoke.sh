#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 The RadioFoundry Authors
# The C4FM build's menus (docs/spec/c4fm.md 6) in the simulator built with CONFIG_C4FM (uiSimC4fm): each page and
# screen opens and draws at every display class. Menu: 0 Switch to VFO/Memory, 1 Zones, 2 Channels, 3 FM, 4 DMR,
# 5 C4FM, 6 Tools, 7 Settings
set -e
fail=0
check() {	# script, expected screen
	for g in 240x320 160x128 160x120i5 128x64; do
		if ! ./uiSimC4fm --geom "$g" --script "$1 expect:screen=$2" --out /dev/null >/dev/null 2>&1; then
			echo "FAIL c4fm menu [$g]: '$1' should reach $2"
			fail=1
		fi
	done
}
check "ok" hub
check "ok 3" group
check "ok 4" group
check "ok 4 down ok" contacts
check "ok 4 down down down ok" recent
check "ok 4 down down down down down down down down down ok" group
check "ok 5" group
check "ok 5 down ok" c4fm-heard
check "ok 5 down down down ok" c4fm-call
check "ok 5 down down down down ok" group
# Wires-X: # on Home on a C4FM VFO (Switch to VFO, Quick tile 4 = mode -> C4FM) opens its page; a room and Connect
check "ok ok quick 4 back ctx" group
check "ok ok quick 4 back ctx down down ok 2 8 6 1 7 ok down ok" group
check "ok 7" settings
check "ok 7 4" group
check "ok 1" zones
check "ok 6" tools
check "ok ok" home
[ $fail = 0 ] && echo "c4fmUiSmoke: passed" || { echo "c4fmUiSmoke: FAILED"; exit 1; }
