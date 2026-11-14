<!-- SPDX-License-Identifier: MIT -->
# DM-32UV: C4FM bench and on-air tests

How to install the C4FM build ([c4fm.md](../spec/c4fm.md)) on a DM-32UV and test it against a Yaesu C4FM radio
(FT-70D, FT5D, FTM-510, FT-991A or similar). Commands run from the checkout's root. Lowest power, a free simplex
frequency, the radios a few meters apart or into a dummy load.

**The programming cable disconnects the radio's speaker and microphone.** Use the cable for settings and for reading
the counters, and talk with the cable out. The counters live in RAM: leave the radio on between `begin` and `end`.

## 0. Build and install

Needs Python 3 with pyserial, the C-SKY toolchain (`tools/build-csky-toolchain.sh`) and the programming cable.

The AMBE+2 codec is not in this repository. It comes from your radio's stock firmware: either the stock file
(`DM32.01.01.049.bin` or `DM32.00.01.046HRVocoder.bin`) in `codecs/`, which the build merges by itself
([codecs/README.md](../../codecs/README.md)), or the `program-memory.bin` of a RadioFoundry backup taken while the radio
ran stock firmware (keep it outside the repository: it is a flash dump).

```
make BOARD=dm32 VARIANT=c4fm                                     # + _codec.bin when codecs/ has the stock file
STOCK=/path/to/program-memory.bin tools/c4fm_flash.sh build      # or: merge the codec from a backup, dry run
STOCK=/path/to/program-memory.bin tools/c4fm_flash.sh flash /dev/ttyUSB0
```

`tools/c4fm_flash.sh build` builds, merges the codec and checks the image without touching the radio (expected:
`RadioFoundry codec slot at 0x030E8000: present`, 974 blocks). `flash` does the same, then writes it through the
radio's update loader: radio off, hold PTT + SK1, switch on. About 2.5 minutes; the radio starts the new image by
itself. The loader answers once per power-on: after an interrupted run, switch off and enter update mode again. The
loader itself is never written, so update mode always stays.

Back to plain RadioFoundry: the same steps from the upstream checkout. Back to stock: Restore from the whole backup
zip (calibration included) in the RadioFoundry programming utility.

## 1. Set-up

- Menu > C4FM > Callsign: your callsign (or a radio name starting with it). Menu > C4FM > DG-ID: 0.
- `export RFOUNDRY_PORT=/dev/ttyUSB0` (or `-p PORT` on any command below).
- With the cable in: `python3 tools/c4fm_bench.py setup --qrg 433.4250 --call N0CALL --power L` sets the VFO to the
  frequency, mode C4FM and power, shows the callsign, radio ID and DG-ID the radio sends, and warns when the callsign
  isn't the one given. Use a free simplex frequency of your band plan, not a calling channel.
- The other radio: DN, DG-ID 00, low power, same frequency.

## 2. One test: begin, talk without the cable, end

```
python3 tools/c4fm_bench.py begin t1-rx-near
#   unplug the cable -> the transmission -> plug it back in
python3 tools/c4fm_bench.py end --note "score 3, metallic"
```

`end` prints the receive and transmit figures with a verdict and saves a JSON record in `../bench-logs/<date>/`
(outside the repository). In `--note`: a score 1-5 (1 unusable, 5 as good as two Yaesu radios) and what it sounds
like ("metallic", "choppy", "noisy", "distorted", "robotic"). Read the same text at each transmission, about 20 s,
at the same level and distance from the microphone. `report` collects the day's records into one Markdown file.

| Command | |
|---|---|
| `setup --qrg MHZ --call CALL [--power L\|M\|H]` | VFO, mode C4FM, power; shows and checks the callsign and DG-ID |
| `status [--all]`, `watch` | the state in two lines (every counter with `--all`); live per second |
| `begin LABEL`, `end --note "..."` | counters before and after a test; `end` warns when the radio restarted in between |
| `wx [--save LABEL]` | Wires-X: the node's last reply and the raw bytes of the last data exchange heard |
| `fields [--compare FILE] [--save LABEL]` | the last call's FICH, CSD1 / CSD2 and DCH per FN in hex and text (`_` space, `<00>` other bytes) |
| `knobs`, `set mic-gain N`, `set nr on\|off`, `set agc on\|off`, `set tone on\|off`, `set level PCT` | the voice and TX settings, RAM only |
| `report` | the day's records as one Markdown file |

The same voice and TX settings are in the radio's menus (Settings > Digital voice, Menu > C4FM > Advanced) for tests
without a computer.

## 3. Tests

### T1. Receive (the Yaesu radio transmits, the DM-32 listens)

| Test | |
|---|---|
| `t1-rx-near` | the other radio at low power, 2-3 m |
| `t1-rx-far` | from another room, the signal clearly weaker |
| `t1-rx-repeater` | a transmission through a repeater |

The receive verdict, from the raw bit error rate:

| BER | Meaning |
|---|---|
| < 0.5 % | clean channel: poor sound is the codec or the audio path, not the receiver |
| 0.5-2 % | the unprotected voice bits get hit: audio degraded |
| > 2 % | the receiver is the limit (signal level, demodulator) |

### T2. Transmit (the DM-32 transmits, the Yaesu radio listens)

One change per test, set before `begin` (from the menus or with `set`):

| Test | Setting |
|---|---|
| `t2-default` | none |
| `t2-mic3`, `t2-mic7` | mic gain 3 / 7 (5 is unity, 2 dB a step) |
| `t2-nr-off` | mic noise reduction off |
| `t2-agc-on` | mic AGC on |
| `t2-tone` | test tone on: a tone stepping in pitch instead of the mic; PTT 10 s, say nothing |
| `t2-dev90`, `t2-dev110` | Menu > C4FM > Advanced > TX deviation 90 / 110 % (100 % = +-2700 Hz) |

A clean tone with poor voice points before the vocoder (mic, level); a poor tone points at the bits or the deviation.
The transmit verdict: padded over 2 % is choppy audio; a mic peak over -1 dBFS is clipping, under -30 dBFS too quiet.

### T3. Fields and callsign

`begin t3-fields`, the other radio transmits 3 s, `end`: the record keeps its fields. `fields` shows the last call's.
Through a repeater and Wires-X: check that the network shows the DM-32's callsign.

### T5. Wires-X

1. On the node's repeater (a C4FM channel with its offset), # on Home: the radio sends a status request and the
   Wires-X page shows the node's name and its room within a few seconds ("no answer" after 6 s).
2. Room ID, Connect: the Room row shows the room; Disconnect: "none".
3. Settling the command format against a Yaesu radio (the DM-32 receives only): put the DM-32 on the repeater's input
   frequency (simplex, no offset), press the Yaesu radio's Wires-X key, then `python3 tools/c4fm_bench.py wx --save
   ft70d-dx`; the same for a connect to a room. The data rows are the Yaesu radio's command, byte for byte. On the
   output frequency the same command shows the node's reply.

### T4. Receiver captures (receive only)

`tools/c4fm_capture.py monitor --minutes 30` logs every transmission heard (callsign, radio ID, frames, bit error
rates, RSSI) and saves the receiver's input audio around the start of each (`.raw`, int16 8 kHz, plus `.json`).
`tests/host/c4fmReplay FILE.raw [--soft]` runs the radio's receiver on a capture on the host, so receiver changes can
be measured on real signals before they are flashed. Nothing in this test transmits.

## 4. Results

| Date | Test | Result | Notes |
|---|---|---|---|
| 2026-11-13 | RX / TX DN with an FT5D, direct | pass | callsign shown both ways |
| 2026-11-13 | QSO through a repeater | pass | |
| 2026-11-13 | Callsign through a Wires-X repeater | fail | wrong callsign there, fine in direct |
| 2026-11-14 | Callsign with the FT-70D field layout (CM 1, `*****` + radio ID, FN 5-6) | pass | callsign shown correctly through Wires-X |
| 2026-11-14 | RX DN from an FT-70D, direct, 2-3 m | pass | BER 0.32 %, FICH bad 1.3 %; score 4-5 |
| 2026-11-14 | Wires-X from the DM-32 to an MMDVM hotspot (YSFGateway) | pass | status, connect and the reply parsed; a Yaesu node not tried yet |
| 2026-11-14 | RX through a repeater (5 transmissions captured) | poor | BER 5-8 %: noise; no ISI, timing and DC fine; the RX filter register doesn't change it (the audio copy is taken before it). Soft decisions on the captures: FICH 50-60 % -> 90-100 % good, DCH CRC nearly all good |
| 2026-11-13 | Voice quality, both ways | fair | sounds like an FT-991A rather than an FT5D |
