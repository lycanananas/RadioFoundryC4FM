# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## title: C4FM
## summary: The C4FM proof of concept: one VFO in FM or C4FM (Yaesu System Fusion DN), callsign and DG-ID.
##
## make BOARD=<board> VARIANT=c4fm. The GPL fork's build (docs/spec/c4fm.md): C4FM on, and what a C4FM / FM radio
## doesn't need off, which also frees the RAM C4FM takes. DMR itself stays built: C4FM shares its AMBE+2 codec, the
## voice path and the modem glue; DMR channels and the DMR VFO are just not offered.

CONFIG_C4FM ?= y

# The VFO in FM or C4FM; no DMR in it
CONFIG_VFO_DMR ?= n

# DMR services C4FM has no use for
CONFIG_DMR_PRIVACY ?= n
CONFIG_DMR_TALKER_ALIAS ?= n
CONFIG_DMR_GPS ?= n
CONFIG_DMR_SMS ?= n
CONFIG_DMR_ARS ?= n
CONFIG_DMR_LRRP ?= n
CONFIG_DMR_SIGNALING ?= n
CONFIG_DMR_EMERGENCY ?= n
CONFIG_DMR_ROAMING ?= n
CONFIG_DMR_IP_GATEWAY ?= n
CONFIG_DMR_CONTROL_STATION ?= n
CONFIG_ZONE_SHARE ?= n
CONFIG_SFR ?= n
CONFIG_IDDB_COMPACT ?= n

# The hotspot and the network services on the programming cable
CONFIG_HOTSPOT ?= n
CONFIG_NET_PPP ?= n
CONFIG_SNMP ?= n

# APRS shares the HR_C7000's FM receive audio copy with the C4FM receiver
CONFIG_APRS ?= n

# Receivers and extras this proof of concept leaves out
CONFIG_DUAL_WATCH ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
CONFIG_LANG_ES ?= n
CONFIG_LANG_IT ?= n
CONFIG_LANG_ZH ?= n
