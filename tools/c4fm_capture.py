#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""c4fm_capture: the C4FM receiver's bench capture over the debug link (docs/spec/c4fm.md 8). Receive only: nothing
here keys the radio.

    c4fm_capture.py monitor [--minutes M] [--out DIR]   log every transmission heard (counters per over) and save the
                                                        receiver's input audio of each (2 s around its start)
    c4fm_capture.py grab [--out DIR]                    freeze the ring now and save it
    c4fm_capture.py analyze FILE.json                   the saved capture's figures (levels, DC, frames)

Each capture: NAME.raw (int16 little endian, 8 kHz, the discriminator as the receiver saw it), NAME.json (the radio's
frame records and counters). Default directory: bench-logs/<date>/capture next to the checkout (outside git).
-p/--port or $RFOUNDRY_PORT picks the cable.
"""
import argparse
import datetime
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rfoundry_debug as rd  # noqa: E402

ROOT = os.path.dirname(HERE)
CAP_RUN, CAP_ARMED, CAP_TRIGGERED, CAP_FROZEN = range(4)
FRAME_FMT = "<IHBBhhBBBB"		# c4fm_cap_frame_t
COUNTERS = ["rx_frames", "rx_fich_ok", "rx_fich_bad", "rx_fich_fec", "rx_vch_errors", "rx_ambe", "dsp_syncs",
            "dsp_losts", "dsp_flywheels"]


def num(v, d=0):
    try:
        return int(str(v), 0)
    except (TypeError, ValueError):
        return d


def out_dir(args):
    d = args.out or os.path.join(os.path.dirname(ROOT), "bench-logs", datetime.date.today().isoformat(), "capture")
    os.makedirs(d, exist_ok=True)
    return d


def cap_view(dl):
    return dl.view(["c4fm_cap"])


def download(dl, cv):
    """(samples oldest first as bytes, frame records oldest first)"""
    n, addr, head = num(cv["cap.n"]), num(cv["cap.samples"]), num(cv["cap.head"])
    raw = dl.dump(addr, 2 * n)
    raw = raw[2 * head:] + raw[:2 * head]
    fn, fsz, faddr, fhead = num(cv["cap.frame_n"]), num(cv["cap.frame_size"]), num(cv["cap.frames"]), num(cv["cap.frame_head"])
    fr = dl.dump(faddr, fn * fsz)
    frames = []
    total = min(fhead, fn)
    for k in range(total):
        i = (fhead - total + k) % fn
        ms, sample, se, flags, gain, center, fi, fnn, fec, vch = struct.unpack_from(FRAME_FMT, fr, i * fsz)
        # position in the reordered samples
        pos = (sample - head) % n
        frames.append({"ms": ms, "pos": pos, "sync_errors": se, "fich": bool(flags & 1), "flywheel": bool(flags & 2),
                       "inverted": bool(flags & 4), "gain": gain, "center": center,
                       "fi": None if fi == 0xFF else fi, "fn": None if fnn == 0xFF else fnn, "fec": fec, "vch": vch})
    return raw, frames


def save_capture(args, dl, cv, label, extra):
    raw, frames = download(dl, cv)
    d = out_dir(args)
    name = f"{time.strftime('%H%M%S')}-{label}"
    with open(os.path.join(d, name + ".raw"), "wb") as f:
        f.write(raw)
    meta = {"label": label, "time": datetime.datetime.now().isoformat(timespec="seconds"), "rate": 8000,
            "trigger_ms": num(cv["cap.trigger_ms"]), "frozen_ms": num(cv["cap.frozen_ms"]), "frames": frames}
    meta.update(extra)
    with open(os.path.join(d, name + ".json"), "w") as f:
        json.dump(meta, f, indent=1)
    return os.path.join(d, name)


def summary(frames):
    use = [f for f in frames if not f["flywheel"]]
    if not frames:
        return "no frames"
    fich = sum(f["fich"] for f in frames)
    fec = sum(f["fec"] for f in frames if f["fich"])
    vch = sum(f["vch"] for f in frames)
    gains = [f["gain"] for f in use] or [0]
    cents = [f["center"] for f in use] or [0]
    se = [f["sync_errors"] for f in use] or [0]
    return (f"{len(frames)} frames, fich ok {fich}, fich fec {fec} ({100.0 * fec / max(1, 200 * fich):.2f}%), "
            f"vch fixed {vch}, sync err avg {sum(se) / len(se):.1f} max {max(se)}, gain {min(gains)}..{max(gains)}, "
            f"center {min(cents)}..{max(cents)}")


def cmd_monitor(dl, args):
    end = time.time() + args.minutes * 60
    d = out_dir(args)
    log = open(os.path.join(d, "monitor.log"), "a")

    def say(s):
        line = f"{time.strftime('%H:%M:%S')} {s}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    dl.setting("c4fm_capture", 1)
    say(f"monitor {args.minutes} min, captures in {d}")
    before, in_over, t0, src, rssi = None, False, 0.0, "", []
    while time.time() < end:
        try:
            v = dl.view(["c4fm"])
            phase = num(v.get("c4fm.phase"))
            rx = phase in (1, 2)
            if rx and not in_over:
                in_over, t0, rssi = True, time.time(), []
                before = before or v
            if rx:
                src = v.get("c4fm.src", "") or src
                r = num(dl.view(["core"]).get("rssi_dbm"), None)
                if r is not None:
                    rssi.append(r)
            if not rx and in_over:
                in_over = False
                dd = {k: num(v.get("c4fm." + k)) - num(before.get("c4fm." + k)) for k in COUNTERS}
                ok = max(1, dd["rx_fich_ok"])
                say(f"OVER {src or '?'} id {v.get('c4fm.src_id') or '-'} dgid {v.get('c4fm.dgid')} "
                    f"{time.time() - t0:.1f}s: frames {dd['rx_frames']}, fich bad {dd['rx_fich_bad']}, "
                    f"ber fich {100.0 * dd['rx_fich_fec'] / (200 * ok):.2f}%, "
                    f"ber vch {100.0 * dd['rx_vch_errors'] / max(1, 81 * dd['rx_ambe']):.2f}%, "
                    f"losts {dd['dsp_losts']}, flywheels {dd['dsp_flywheels']}, gain {v.get('c4fm.rx_gain')}, "
                    f"center {v.get('c4fm.rx_center')}, inverted {v.get('c4fm.rx_inverted')}, "
                    f"rssi {min(rssi) if rssi else '?'}..{max(rssi) if rssi else '?'} dBm")
                before, src = v, ""
            if not in_over:
                before = v
            cv = cap_view(dl)
            if num(cv.get("cap.state")) == CAP_FROZEN:
                path = save_capture(args, dl, cv, "over", {"src": v.get("c4fm.src", "")})
                with open(path + ".json") as f:
                    say(f"CAPTURE {os.path.basename(path)}: {summary(json.load(f)['frames'])}")
                dl.setting("c4fm_capture", 1)
            elif num(cv.get("cap.state")) == CAP_RUN:
                dl.setting("c4fm_capture", 1)
        except rd.DebugLinkError as e:
            say(f"debug link: {e}")
            time.sleep(2)
        time.sleep(0.3)
    say("monitor done")


def cmd_grab(dl, args):
    dl.setting("c4fm_capture", 2)
    time.sleep(0.2)
    cv = cap_view(dl)
    path = save_capture(args, dl, cv, "grab", {})
    with open(path + ".json") as f:
        print(f"{path}: {summary(json.load(f)['frames'])}")
    dl.setting("c4fm_capture", 0)


def cmd_analyze(args):
    import numpy as np
    meta = json.load(open(args.file))
    x = np.frombuffer(open(args.file[:-5] + ".raw", "rb").read(), dtype="<i2").astype(float)
    print(summary(meta["frames"]))
    print(f"samples {len(x)}, mean {x.mean():.0f}, rms {x.std():.0f}, min {x.min():.0f}, max {x.max():.0f}, "
          f"clipped {(np.abs(x) >= 32767).sum()}")
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / 8000)
    bands = [(0, 300), (300, 1000), (1000, 2000), (2000, 2500), (2500, 3000), (3000, 4000)]
    tot = spec.sum()
    print("power by band: " + ", ".join(f"{a}-{b} Hz {100 * spec[(f >= a) & (f < b)].sum() / tot:.1f}%"
                                        for a, b in bands))


def main():
    ap = argparse.ArgumentParser(description="C4FM bench capture (receive only)", epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default=os.environ.get("RFOUNDRY_PORT"))
    ap.add_argument("--out")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("monitor")
    m.add_argument("--minutes", type=float, default=30)
    sub.add_parser("grab")
    a = sub.add_parser("analyze")
    a.add_argument("file")
    args = ap.parse_args()
    if args.cmd == "analyze":
        cmd_analyze(args)
        return
    try:
        with rd.DebugLink(args.port) as dl:
            if dl.view(["c4fm_cap"]).get("cap.n") is None:
                sys.exit("this firmware has no C4FM capture (flash the current build)")
            {"monitor": cmd_monitor, "grab": cmd_grab}[args.cmd](dl, args)
    except rd.DebugLinkError as e:
        sys.exit(f"debug link: {e}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
