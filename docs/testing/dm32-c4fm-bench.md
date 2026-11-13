<!-- SPDX-License-Identifier: MIT -->
# DM-32UV: C4FM bench procedure

First on-air checks of the C4FM fork ([c4fm.md](../spec/c4fm.md)) on a DM-32UV, with a Yaesu C4FM radio (FT5D,
FT-991A or similar) as the other station. Lowest power, a simplex test frequency, the radios a few meters apart or
into a dummy load.

## 0. Build and install

1. Copy `DM32.01.01.049.bin` (stock firmware) into `codecs/` ([codecs/README.md](../../codecs/README.md)).
2. `make BOARD=dm32 VARIANT=c4fm` -> `build/dm32-c4fm/rfoundry_dm32_codec.bin` (with the codec merged in).
3. Install as any DM-32 build ([hardware-dm32.md](../spec/hardware-dm32.md) 13, `tools/rfoundry`):
   `rfoundry flash build/dm32-c4fm/rfoundry_dm32_codec.bin --board dm32`.
4. Settings > My radio > DMR ID and name: the radio name starts with your callsign (`SP9ABC Name`). Settings > My radio
   > C4FM DG-ID: 0.
5. VFO, the test frequency, Quick (*) > Mode until it says C4FM. Home's talk row shows `SP9ABC DG-ID 00`.

The debug link (`tools/rfoundry_debug.py`, programming cable) is used below; `view --section dmr_ext` gives the
`c4fm.*` keys.

## 1. Receive (FT5D transmits DN, DG-ID 00)

| Check | Pass | If not |
|---|---|---|
| `c4fm.dsp_syncs` rises during a transmission | yes | no audio reaches the receiver: `c4fm.rx_gain` near 0. Check FM_BANDWIDTH (`reg 1 0x00 --page 5`), try 0x4F and 0x5F; the FD6818's AF register 0x47 (`reg 0 0x47`) |
| `c4fm.rx_fich_bad` small against `rx_frames` | under 10 % | syncs but bad frames: the audio isn't flat (de-emphasis). Note `rx_gain`, `rx_center`; record the FM receive copy (open question 2) |
| Home shows the FT5D's callsign, voice plays | yes | frames good, no voice: `c4fm.rx_ambe` rising? then the vocoder / speaker path (`dmr.codec=1`?) |
| `c4fm.rx_inverted` | either | informational: the receiver handles both |
| DG-ID: FT5D on DG-ID 05, ours 00, then ours 05, then ours 07 | plays, plays, busy | |

## 2. Transmit (FT5D receives DN, DG-ID 00 or AMS)

| Check | Pass | If not |
|---|---|---|
| PTT: `c4fm.drv_keyups` +1, `drv_slots` rising, `drv_stalls` 0 | yes | no slot interrupts in the physical layer mode: `drv_ints` (PHY_RDY, timeslot, RF TX) |
| `c4fm.drv_idle` stays near 1-2 per transmission (the tail) | yes | the stream runs dry: `c4fm.tx_padded` high = the mic / vocoder isn't delivering in this mode (open question 3) |
| FT5D shows our callsign and DN, voice clear | yes | FT5D silent: deviation. Step the level: `reg 2 0 120`, `reg 2 0 160` (percent of DMR's; default 139) and key again. Garbled: same, and note `c4fm.drv_regs` |
| Deviation on a service monitor or SDR | outer +-2.7 kHz | adjust the level as above; RF_MOD_BIAS_CTRL can be written live while keyed: `reg 1 0x14 VALUE --page 1` |

Record per test: the level, `c4fm.drv_regs`, the FT5D's display, and whether the audio was clear.

## 3. Results

| Date | Test | Result | Notes |
|---|---|---|---|
| | | | |
