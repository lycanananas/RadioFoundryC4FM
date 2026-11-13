<!-- SPDX-License-Identifier: MIT -->
# C4FM (Yaesu System Fusion DN voice)

The C4FM fork's radio mode: DN voice (V/D mode 2) beside FM, on one frequency. Proof of concept, untested on air.

| Contents | |
|---|---|
| 1 | Scope and license |
| 2 | Why the radio's DMR parts carry C4FM |
| 3 | Layer 2 (core/c4fm/ysf.c) |
| 4 | Receiver and modulator (core/c4fm/c4fm_dsp.c) |
| 5 | Calls (core/c4fm/c4fm_call.c) |
| 6 | Radio core, settings, UI |
| 7 | The board's modem (hal_c4fm.h) |
| 8 | Diagnostics and bench registers |
| 9 | Open questions |

## 1. Scope and license

| Item | |
|---|---|
| Voice | DN only: AMBE+2 3600x2450, the frame DMR carries. VW (voice full rate, IMBE) and V/D mode 1 are shown, not played |
| Identity | A callsign: the radio name's first word (`radio_cwid_callsign`). No contacts, no talk groups, no numeric ID |
| Selector | DG-ID 0-99 (Settings > My radio > C4FM DG-ID). 00 sends with DG-ID 0 and hears every transmission |
| Not done | Wires-X, GPS (DT1 / DT2), data FR mode, picture and text, repeater (Fusion link) features, C4FM channels in the codeplug: the VFO's mode is the one way in |
| License | `core/c4fm/*` and its tests are GPL-2.0-or-later: the layout facts come from MMDVMHost and MMDVM_CM. The rest of the change is MIT, but a build with `CONFIG_C4FM=y` is a GPL work, and none of it can go upstream (docs/CLEANROOM.md) |

## 2. Why the radio's DMR parts carry C4FM

| C4FM | DMR | So |
|---|---|---|
| 4FSK, 4800 Bd, root raised cosine alpha 0.2 | same | the DMR modem's physical layer modulates C4FM bits |
| dibits 01 +3, 00 +1, 10 -1, 11 -3 | same | the bits go to the modem as they are |
| outer deviation +-2700 Hz, inner +-900 Hz | +-1944 / +-648 Hz | modulation amplitude x 1.39 while transmitting C4FM |
| DN voice: AMBE+2 2450 bit/s, 49 parameter bits per 20 ms | same codec rate and parameter order | the radio's own DMR codec (hal_vocoder_t) speaks C4FM DN; only the FEC around the 49 bits differs |
| 100 ms frames, continuous | 30 ms TDMA slots | the stream is cut into slots back to back (section 7) |

The 49 parameter bits are in the DMR stack's order (`ambeFec.h`): bits 0-11 Golay(24,12) data, 12-23 Golay(23,12)
data, 24-48 unprotected. C4FM sends the same words: MMDVM_CM converts DMR to DN voice by re-coding the FEC only.

## 3. Layer 2

One frame: 120 bytes, 960 bits, 480 symbols, 100 ms. Bits numbered from the MSB of byte 0.

| Bytes | Part | Coding |
|---|---|---|
| 0-4 | sync `D4 71 C9 63 4D` | 20 symbols |
| 5-29 | FICH | 32 bits + CRC-16 -> 4 x Golay(24,12) -> 96 + 4 tail -> rate 1/2 K=5 -> 200 bits, dibit interleave 20 x 5 |
| 30-119 | payload | five 144-bit blocks |

| Payload | Per block | Channel |
|---|---|---|
| Header, terminator | 72 bits CSD1 + 72 bits CSD2 | each: 20 bytes, whitened, CRC-16, rate 1/2 (180 symbols), interleave 20 x 9. CSD1 = destination + source, CSD2 = downlink + uplink (10 characters each) |
| Communications, DN | 40 bits DCH + 104 bits VCH | DCH: 10 bytes, whitened, CRC-16, rate 1/2 (100 symbols), interleave 20 x 5. VCH: 49 bits, bits 0-26 three times, 27-48 once, one zero bit; whitened; bit interleave 26 x 4 |

FICH fields: FI (0 header, 1 communications, 2 terminator), CS 2, CM 0 (group), BN / BT 0, FN 0..FT, FT 6, DEV 0,
MR 0 (direct), DT 2 (DN), DG-ID. CRC-16: polynomial 0x1021, init 0, inverted, big endian. Convolutional code:
G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4. The whitening sequence and the interleave tables are in `ysf.c`.

DCH by FN (FT 6): 0 destination (`ALL`), 1 source (our callsign), 2 downlink, 3 uplink, 4-5 blank, 6 data (zeros).

`tests/host/c4fmYsfTest.c` checks every part bit for bit against vectors generated from MMDVM_CM's encoder
(`c4fmOracle.h`).

## 4. Receiver and modulator

| Step | |
|---|---|
| Input | discriminator audio, 8 kHz, 16 bit (the board's modem, section 7) |
| Matched filter | interpolation by 3 through the RRC (alpha 0.2, 8 symbols, 41 taps): 24 kHz, 5 samples per symbol. 8 kHz is enough: the signal's band ends at 2.88 kHz |
| Search | every sample: the last 20 symbols' samples correlated with the sync, DC removed, normalized by their mean absolute deviation. Above 0.75 of the ideal: the best point of the next 5 samples is the frame start |
| Levels | least squares fit of the 20 sync samples to their levels: gain and center. A negative gain is an inverted discriminator; nothing else changes |
| First lock | at most 4 of the 40 sync bits wrong |
| Tracking | the next sync is looked for within +-2 samples of where it is due, above 0.5 of the ideal and within 6 dB of the last level |
| Loss | a missed sync is bridged on the old timing (flywheel frame) up to 3 times, then the receiver searches again |

Integer arithmetic only. About 14 multiply-adds per 24 kHz sample for the filter, 20 more while searching.
`c4fmDspTest.c`: frames bit exact through timing offsets, DC, inversion, a 300-unit level; the FICH survives noise at
0.35 of the inner level; no false frame in 10 s of noise.

The modulator (RRC at 24 kHz, decimated to 8 kHz) serves the host tests.

## 5. Calls

| Phase | Transmit | Receive |
|---|---|---|
| Start | 100 ms of `0x77` (+3 -3 ...), header (CSD1 `ALL` / our callsign) | a header, or a communications frame whose FICH decodes (late entry) |
| Voice | a frame per five voice frames; DCH cycling by FN | DN: DCH by FN (FN 1 the caller's callsign), five AMBE frames to the voice path. Another DT: shown, not played |
| Gaps | the modem kept 100-200 ms ahead; a frame still short of voice when the modem runs low is padded with silence | a frame whose FICH fails inside a call is taken as the communications frame due |
| End | PTT up: the voice left (padded), the terminator, unkey once it has played out | the terminator, a lost signal, or 400 ms without a frame |

DG-ID: with ours at 0 every transmission plays; else only those carrying ours (the rest show as busy).

The voice path is the DMR stack's (`ambeVoice`: vocoder, PCM, concealment). The engine converts 49 bits to and from
the 72-bit DMR air frame with `ambeFec`, which is lossless both ways. `c4fmCallTest.c`: two radios through the
modulator and receiver; every voice frame arrives in order and unchanged, the header and late entry give the caller,
another DG-ID doesn't play.

## 6. Radio core, settings, UI

| Item | |
|---|---|
| Mode | `RADIO_MODE_C4FM`, offered with `board.c4fm`. The VFO's Mode tile cycles FM -> C4FM (-> DMR where the build's VFO has it) |
| Board | `set_mode(C4FM)`: the RF side as for DMR, then the C4FM modem. PTT keys the board as for DMR (`key_tx` at 12.5 kHz); the engine starts the stream; the core unkeys once the engine is off the air |
| State | RX / TX follow the engine's phase; the time-out stops the transmission with its terminator |
| Settings | `settings_t.c4fm_dgid` (was `reserved9`: an older record reads 0) |
| Home | talk row: our callsign and DG-ID, or the caller's while receiving; chips: DN, power, 12.5k; activity: the caller and the mode heard, then "Last heard" |
| Build | `CONFIG_C4FM` (needs `CONFIG_DMR`); `make VARIANT=c4fm` turns it on and leaves out what a C4FM / FM radio doesn't need |

## 7. The board's modem

`hal_c4fm.h`: a byte stream at 9600 bit/s out (DMR dibit mapping, C4FM deviation, idle `0x77` when the queue runs
dry), discriminator audio in at `rx_rate_hz`. The HR_C7000 version (`drivers/modem/hrc7000/hrc7000_c4fm.c`):

| Direction | How |
|---|---|
| Transmit | the physical layer continuous mode (WORK_MODE 0x02, LAYER2_CONTROL 0x82), 36 bytes per 30 ms slot to both TX RAM halves at each slot boundary (the first PHY_RDY or timeslot interrupt), as the split duplex TX half does ([modem-hrc7000.md](modem-hrc7000.md) 2.10). RF_MOD_BIAS_CTRL's two modulation amplitudes x 1.39 while keyed. RF chip and PA keyed once at the first RF TX interrupt; 200 ms without a slot interrupt drops the PA |
| Receive | the chip in FM copies its AF input to the RX RAM at 8 kHz ([modem-hrc7000.md](modem-hrc7000.md) 8.5), the voice band-pass off (FM_BANDWIDTH 0x0F); the RF chip in its DMR set-up (flat discriminator). FM playback off: the speaker gets the decoded voice through the prompt FIFO |
| Speaker | the prompt FIFO also feeds the modulator: its gate closes while C4FM transmits (`hrc_dmr_tx_active`) |

## 8. Diagnostics and bench registers

Debug link view, section `dmr_ext`:

| Key | |
|---|---|
| `c4fm.phase`, `src`, `dst`, `dgid`, `dt`, `late` | the engine (phase: 0 idle, 1 RX voice, 2 RX busy, 3 TX, 4 TX ending, 5 denied) |
| `c4fm.dsp_syncs`, `dsp_losts`, `dsp_flywheels` | syncs found, signals lost, frames bridged |
| `c4fm.rx_gain`, `rx_center`, `rx_inverted` | the last level fit: gain per level unit (inner = 1), DC, polarity |
| `c4fm.rx_frames`, `rx_fich_bad`, `rx_vch_errors`, `rx_ambe` | frames taken, FICH failures, voice triplets corrected, voice frames played |
| `c4fm.tx_frames`, `tx_padded` | frames sent, voice frames padded with silence |
| `c4fm.drv_*` | the driver: key-ups, slots, slots from the stream / with idle, stalls, interrupts, level, RF_MOD_BIAS_CTRL for DMR and for C4FM |

CHIP_REG (`rfoundry_debug.py reg CHIP REG [VALUE] --page PAGE`), boards that build C4FM:

| Chip | Registers |
|---|---|
| 1 | the HR_C7000's modem registers, offset = page << 8 \| reg: RF_MOD_BIAS_CTRL 0x114 (live, also while keyed), RF_MODE 0x104 (b23:16 AF gain), FM_BANDWIDTH 0x500 (receive filters, rewritten at every receive set-up) |
| 2 | register 0: the C4FM TX level in percent of DMR's modulation (139), from the next key-up |

## 9. Open questions

1. The deviation the scaled RF_MOD_BIAS_CTRL gives: linear? Measured against a C4FM receiver's or a service monitor's.
2. Whether the FM receive copy is flat enough with the band-pass off: de-emphasis and the 3 kHz low-pass are still in
   the chain. If not: the chip's own DMR demodulator in the physical layer receive mode with the C4FM sync, or a
   de-emphasis inverse in the receiver.
3. Whether mic PCM and the vocoder keep their timing in the physical layer mode (the split duplex TX half fed its
   voice from the host).
4. C4FM receivers' lock on our header after 100 ms of preamble (Yaesu radios key with a longer one).
