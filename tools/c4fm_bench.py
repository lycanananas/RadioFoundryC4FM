#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""c4fm_bench: the C4FM bench session over the debug link (docs/spec/c4fm.md 8, docs/testing/dm32-c4fm-bench.md).

Records one over at a time (counters before and after, the fields heard, the voice settings in force), works out
what they mean, and keeps every record as JSON in a log directory outside the repository, for the report.

    c4fm_bench.py setup --qrg MHZ [--call CALL] [--power L|M|H]
                                              VFO on the test frequency, mode C4FM, power; checks our callsign
    c4fm_bench.py status [--all]              the C4FM state now (--all: every counter)
    c4fm_bench.py watch [--interval S]        live: phase, caller, frames, error rates
    c4fm_bench.py begin LABEL                 counters before a test (then unplug the cable: it cuts the radio's
                                              speaker and mic; talk; plug it back in, radio left on)
    c4fm_bench.py end [--note TEXT]           counters after: the record (receive and transmit), saved
    c4fm_bench.py fields [--compare FILE]     the last call's fields (FICH, CSD, DCH) decoded, byte for byte
    c4fm_bench.py knobs                       the voice and TX settings in force
    c4fm_bench.py set mic-gain 0..10          mic gain step (5 = unity, 2 dB a step), RAM only
    c4fm_bench.py set nr on|off               vocoder noise reduction, RAM only
    c4fm_bench.py set agc on|off              vocoder input level control, RAM only
    c4fm_bench.py set tone on|off             test tone into the encoder instead of the mic, RAM only
    c4fm_bench.py set level PCT               C4FM TX deviation, percent of DMR's (139 = +-2700 Hz), next key-up
    c4fm_bench.py report [--day YYYY-MM-DD]   every record of a day as one Markdown file (to hand over)

-p/--port DEV (anywhere on the line, or $RFOUNDRY_PORT) picks the programming cable, e.g. -p /dev/ttyUSB1;
--logs DIR the log directory (default: bench-logs next to the checkout, outside git). Needs pyserial.
"""
import argparse
import datetime
import json
import math
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rfoundry_debug as rd  # noqa: E402

ROOT = os.path.dirname(HERE)
DEFAULT_LOGS = os.path.join(os.path.dirname(ROOT), "bench-logs")

SECTIONS = ["core", "features", "c4fm", "c4fm_raw", "dmr"]
PHASES = {0: "idle", 1: "rx voice", 2: "rx busy", 3: "tx", 4: "tx ending", 5: "tx denied"}
DT = {0: "V/D1", 1: "data FR", 2: "DN (V/D2)", 3: "VW"}
FI = {0: "header", 1: "comms", 2: "terminator", 3: "test"}
FICH_KEYS = ["fi", "cs", "cm", "bn", "bt", "fn", "ft", "dev", "mr", "voip", "dt", "sql", "dgid"]
# counters whose difference over an over is meaningful
COUNTERS = ["rx_frames", "rx_fich_ok", "rx_fich_bad", "rx_fich_fec", "rx_calls", "rx_vch_errors", "rx_ambe", "dsp_syncs",
            "dsp_losts", "dsp_flywheels", "tx_calls", "tx_frames", "tx_padded", "drv_keyups", "drv_slots", "drv_sent",
            "drv_idle", "drv_stalls"]
OPT_NR, OPT_TONE, OPT_AGC = 0x01, 0x02, 0x04


def num(v, default=0):
    try:
        return int(str(v), 0)
    except (TypeError, ValueError):
        return default


def snapshot(dl):
    return dl.view(SECTIONS)


def c4fm(view, key, default=0):
    return num(view.get("c4fm." + key), default)


def deltas(before, after):
    return {k: c4fm(after, k) - c4fm(before, k) for k in COUNTERS}


def pct(a, b):
    return 100.0 * a / b if b else None


def fmt(v, unit="%", digits=2):
    return "n/a" if v is None else f"{v:.{digits}f}{unit}"


def mic_dbfs(view):
    peak = num(view.get("voice.tx_mic_peak"))
    return 20 * math.log10(peak / 32768.0) if peak > 0 else None


def knobs(view, dl=None):
    opts = num(view.get("voice.enc_options"))
    k = {
        "mic_gain": view.get("set.mic_gain"),
        "noise_reduction": bool(opts & OPT_NR),
        "agc": bool(opts & OPT_AGC),
        "tone_frames": bool(opts & OPT_TONE),
        "enc_options": view.get("voice.enc_options"),
        "enc_supported": view.get("voice.enc_supported"),
        "enc_flags": view.get("voice.enc_flags"),
        "test_tone": view.get("voice.tx_test"),
        "tx_level_pct": view.get("c4fm.drv_level"),
        "rf_mod_bias": view.get("c4fm.drv_regs"),
        "codec": view.get("dmr.codec"),
    }
    if dl is not None:
        try:
            k["codec_profile"] = dl.version().get("codec")
        except Exception:  # an older firmware: no codec key
            pass
    return k


def rx_metrics(d, after):
    good = d["rx_fich_ok"]
    m = {
        "frames": d["rx_frames"],
        "fich_bad_pct": pct(d["rx_fich_bad"], d["rx_frames"]),
        # FICH: 200 coded bits a frame; the Viterbi decoder's corrections estimate the raw channel bit errors
        "ber_fich_pct": pct(d["rx_fich_fec"], 200 * good),
        # VCH: 27 triplets a voice frame; a corrected triplet had one copy of three wrong
        "vch_triplets_fixed_pct": pct(d["rx_vch_errors"], 27 * d["rx_ambe"]),
        "ber_vch_pct": pct(d["rx_vch_errors"], 3 * 27 * d["rx_ambe"]),
        "voice_frames": d["rx_ambe"],
        "syncs": d["dsp_syncs"], "losts": d["dsp_losts"], "flywheels": d["dsp_flywheels"],
        "gain": c4fm(after, "rx_gain"), "center": c4fm(after, "rx_center"), "inverted": c4fm(after, "rx_inverted"),
        "src": after.get("c4fm.src", ""), "src_id": after.get("c4fm.src_id", ""), "dst": after.get("c4fm.dst", ""),
        "dgid": c4fm(after, "dgid"), "dt": DT.get(c4fm(after, "dt"), c4fm(after, "dt")),
        "late_entry": c4fm(after, "late"),
    }
    ber = m["ber_vch_pct"] if m["ber_vch_pct"] is not None else m["ber_fich_pct"]
    if m["frames"] == 0:
        verdict = "nothing received: no sync (see the bench doc: FM_BANDWIDTH, rx_gain)"
    elif ber is None:
        verdict = "frames but no voice measured"
    elif ber < 0.5:
        verdict = "clean channel: poor sound here is NOT the receiver (vocoder, codec, audio path)"
    elif ber < 2.0:
        verdict = "some bit errors: unprotected voice bits get hit, audio degraded (receiver / signal)"
    else:
        verdict = "many bit errors: the receiver is the problem (signal level, audio flatness, demodulator)"
    m["verdict"] = verdict
    return m


def tx_metrics(d, after):
    voice = 5 * d["tx_frames"]
    m = {
        "frames": d["tx_frames"],
        "padded_pct": pct(d["tx_padded"], voice),
        "drv_idle": d["drv_idle"], "drv_stalls": d["drv_stalls"], "drv_keyups": d["drv_keyups"],
        "drv_slots": d["drv_slots"],
        "mic_peak_dbfs": mic_dbfs(after),
    }
    problems = []
    if m["frames"] == 0:
        problems.append("no frames sent")
    if (m["padded_pct"] or 0) > 2:
        problems.append("voice frames padded with silence: choppy audio (mic / vocoder timing)")
    if d["drv_stalls"]:
        problems.append("slot clock stalled: the PA dropped")
    if d["drv_idle"] > 3:
        problems.append("the stream ran dry inside the over")
    db = m["mic_peak_dbfs"]
    if db is not None and db > -1.0:
        problems.append("mic peak at full scale: clipping, lower the mic gain")
    elif db is not None and db < -30.0:
        problems.append("mic very quiet: raise the mic gain")
    m["verdict"] = "; ".join(problems) if problems else "stream clean: poor sound on the other radio is the vocoder " \
        "input (gain, options) or the deviation (set level)"
    return m


def field_text(b):
    """A field as a readable string: printable ASCII as is, space as '_', anything else as <xx>"""
    out = []
    for c in b:
        if c == 0x20:
            out.append("_")
        elif 0x20 < c < 0x7F:
            out.append(chr(c))
        else:
            out.append(f"<{c:02x}>")
    return "".join(out)


def fields(view):
    raw = {}
    fich = view.get("c4fm.raw_fich", "")
    if fich:
        raw["fich"] = dict(zip(FICH_KEYS, (num(x) for x in fich.split(","))))
    for k in ["hdr_csd1", "hdr_csd2", "term_csd1", "term_csd2"] + [f"dch{i}" for i in range(8)]:
        v = view.get("c4fm.raw_" + k, "-")
        raw[k] = None if v in ("-", "") else v
    return raw


def print_fields(raw, ref=None):
    f = raw.get("fich")
    if f:
        print("FICH  " + " ".join(f"{k}={v}" for k, v in f.items())
              + f"   ({FI.get(f['fi'], '?')}, {DT.get(f['dt'], '?')})")
        if ref and ref.get("fich"):
            diff = [f"{k}: {ref['fich'].get(k)} -> {v}" for k, v in f.items() if ref["fich"].get(k) != v]
            print("      vs reference: " + (", ".join(diff) if diff else "same"))
    names = {"hdr_csd1": "header CSD1 (dst|src)", "hdr_csd2": "header CSD2 (down|up)",
             "term_csd1": "term CSD1 (dst|src)", "term_csd2": "term CSD2 (down|up)"}
    names.update({f"dch{i}": f"DCH FN {i}" for i in range(8)})
    for k, label in names.items():
        h = raw.get(k)
        line = f"{label:24s} "
        if h is None:
            line += "-"
        else:
            b = bytes.fromhex(h)
            text = field_text(b[:10]) + ("|" + field_text(b[10:]) if len(b) > 10 else "")
            line += f"{h}  {text}"
        if ref is not None and k in ref and ref.get(k) != h:
            line += f"\n{'':24s} reference: {ref.get(k) or '-'}  <-- differs"
        print(line)


def print_status(v, full=False):
    good = c4fm(v, "rx_fich_ok")
    print(f"own {v.get('c4fm.own_call') or '(no callsign)'} ID {v.get('c4fm.own_id') or '-'} DG-ID "
          f"{c4fm(v, 'own_dgid'):02d} | {PHASES.get(c4fm(v, 'phase'), '?')} | heard '{v.get('c4fm.src', '')}' ID "
          f"{v.get('c4fm.src_id') or '-'} DG-ID {c4fm(v, 'dgid'):02d} "
          f"{DT.get(c4fm(v, 'dt'), '?')}")
    print(f"rx {c4fm(v, 'rx_frames')} frames, fich bad {c4fm(v, 'rx_fich_bad')}, "
          f"ber~{fmt(pct(c4fm(v, 'rx_fich_fec'), 200 * good))}, gain {c4fm(v, 'rx_gain')} | "
          f"tx {c4fm(v, 'tx_frames')} frames, padded {c4fm(v, 'tx_padded')}, stalls {c4fm(v, 'drv_stalls')}, "
          f"level {v.get('c4fm.drv_level')}%")
    if full:
        for k in COUNTERS:
            print(f"  {k:16s} {c4fm(v, k)}")
        print(f"  rx_center {c4fm(v, 'rx_center')}  rx_inverted {c4fm(v, 'rx_inverted')}  "
              f"drv_regs {v.get('c4fm.drv_regs')}  drv_ints {v.get('c4fm.drv_ints')}")


def log_dir(args, day=None):
    d = os.path.join(args.logs, day or datetime.date.today().isoformat())
    os.makedirs(d, exist_ok=True)
    return d


def save(args, record):
    d = log_dir(args)
    n = len([f for f in os.listdir(d) if f.endswith(".json")]) + 1
    safe = "".join(c if c.isalnum() or c in "-_" else "-" for c in record["label"])[:40]
    path = os.path.join(d, f"{n:03d}-{record['kind']}-{safe}.json")
    with open(path, "w") as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
    return path


def pending_path(args):
    return os.path.join(args.logs, "pending.json")


def cmd_begin(dl, args):
    v = snapshot(dl)
    os.makedirs(args.logs, exist_ok=True)
    with open(pending_path(args), "w") as f:
        json.dump({"label": args.label, "note": args.note or "", "time": time.time(), "before": v}, f)
    print(f"[{args.label}] counters taken. Unplug the cable, do the test (radio stays on), plug it back in, then: end")


def cmd_end(dl, args):
    try:
        with open(pending_path(args)) as f:
            p = json.load(f)
    except FileNotFoundError:
        sys.exit("no test started: begin LABEL first")
    before, after = p["before"], snapshot(dl)
    d = deltas(before, after)
    restarted = any(v < 0 for v in d.values())
    rx = rx_metrics(d, after) if d["rx_frames"] or d["dsp_syncs"] else None
    tx = tx_metrics(d, after) if d["tx_frames"] or d["drv_keyups"] else None
    note = " / ".join(x for x in (p.get("note"), args.note) if x)
    record = {
        "kind": "test", "label": p["label"], "note": note,
        "time": datetime.datetime.now().isoformat(timespec="seconds"),
        "minutes": round((time.time() - p["time"]) / 60, 1), "restarted": restarted,
        "rx": rx, "tx": tx, "deltas": d, "knobs": knobs(after, dl),
        "fields": fields(after) if rx else None,
        "radio": {k: after.get(k) for k in ("rx_hz", "tx_hz", "power", "power_names", "mode", "vfo", "squelch")
                  if k in after},
        "after": after,
    }
    path = save(args, record)
    os.remove(pending_path(args))
    print(f"[{p['label']}] {record['minutes']} min")
    if restarted:
        print("  WARNING: the radio restarted during the test, counters lost (power, cable, crash?)")
    if rx:
        print(f"  RX {rx['frames']} frames, fich bad {fmt(rx['fich_bad_pct'])}, ber {fmt(rx['ber_vch_pct'])} "
              f"(fich {fmt(rx['ber_fich_pct'])}), losts {rx['losts']}, from '{rx['src']}' DG-ID {rx['dgid']:02d}")
        print(f"     -> {rx['verdict']}")
    if tx:
        print(f"  TX {tx['frames']} frames, padded {fmt(tx['padded_pct'])}, idle {tx['drv_idle']}, stalls "
              f"{tx['drv_stalls']}, mic peak {fmt(tx['mic_peak_dbfs'], ' dBFS', 1)}")
        print(f"     -> {tx['verdict']}")
    if not rx and not tx:
        print("  nothing received or sent since begin")
    print(f"  saved {os.path.relpath(path)}")


def snapshot_small(dl):
    return dl.view(["c4fm"])


def cmd_status(dl, args):
    v = snapshot(dl)
    if v.get("c4fm.built") != "1":
        sys.exit("this firmware has no C4FM (c4fm.built != 1)")
    print_status(v, args.all)


def cmd_watch(dl, args):
    prev = None
    while True:
        v = snapshot_small(dl)
        line = f"{time.strftime('%H:%M:%S')} {PHASES.get(c4fm(v, 'phase'), '?'):9s} {v.get('c4fm.src', ''):10s}"
        if prev is not None:
            d = deltas(prev, v)
            good = d["rx_fich_ok"]
            if d["rx_frames"]:
                line += f" | rx {d['rx_frames']} fr, bad {d['rx_fich_bad']}, ber~{fmt(pct(d['rx_fich_fec'], 200 * good), '%', 1)}"
            if d["tx_frames"]:
                line += f" | tx {d['tx_frames']} fr, pad {d['tx_padded']}, idle {d['drv_idle']}"
        print(line, flush=True)
        prev = v
        time.sleep(args.interval)


def power_names(v):
    return [n for n in (v.get("power_names") or "").replace(",", "/").split("/")]


def cmd_setup(dl, args):
    if args.qrg:
        dl.vfo(True)
        time.sleep(0.3)
        dl.vfo_set(round(float(args.qrg) * 1e6))
    dl.dmr_fields(mode="c4fm")
    if args.power:
        names = [n.upper() for n in power_names(dl.view(["core"]))]
        dl.power_level(names.index(args.power.upper()) if args.power.upper() in names else args.power)
    time.sleep(0.5)
    v = snapshot(dl)
    names = power_names(v)
    pw = num(v.get("power"))
    print(f"QRG {num(v.get('rx_hz')) / 1e6:.4f} MHz, mode {v.get('mode')}, power "
          f"{names[pw] if pw < len(names) and names[pw] else pw}")
    print_status(v)
    own = v.get("c4fm.own_call", "")
    if args.call and own.upper() != args.call.upper():
        print(f"WARNING: our callsign is '{own}', not {args.call.upper()}: Settings > My radio > name must start "
              f"with {args.call.upper()}")


def cmd_fields(dl, args):
    v = snapshot(dl)
    raw = fields(v)
    ref = None
    if args.compare:
        with open(args.compare) as f:
            j = json.load(f)
        ref = j.get("fields") or j
    print(f"last call: src '{v.get('c4fm.src', '')}', dst '{v.get('c4fm.dst', '')}', capture #{v.get('c4fm.raw_serial')}")
    print_fields(raw, ref)
    if args.save:
        rec = {"kind": "fields", "label": args.save, "note": "",
               "time": datetime.datetime.now().isoformat(timespec="seconds"), "fields": raw,
               "src": v.get("c4fm.src", ""), "dst": v.get("c4fm.dst", "")}
        print(f"saved {save(args, rec)}")


def cmd_knobs(dl, args):
    for k, v in knobs(snapshot(dl), dl).items():
        print(f"  {k:16s} {v}")


def on_off(s):
    if s not in ("on", "off"):
        sys.exit("on or off")
    return s == "on"


def cmd_set(dl, args):
    v = snapshot(dl)
    opts = num(v.get("voice.enc_options"), OPT_NR | OPT_TONE) | OPT_TONE
    what, val = args.what, args.value
    if what == "mic-gain":
        dl.setting("mic_gain", int(val))
    elif what == "nr":
        dl.setting("enc_options", (opts | OPT_NR) if on_off(val) else (opts & ~OPT_NR))
    elif what == "agc":
        dl.setting("enc_options", (opts | OPT_AGC) if on_off(val) else (opts & ~OPT_AGC))
    elif what == "tone":
        dl.voice_test(on_off(val))
    elif what == "level":
        print(f"TX level now {dl.chip_reg(2, 0, int(val))} % (from the next key-up)")
    else:
        sys.exit(f"unknown setting {what}")
    cmd_knobs(dl, args)


def cmd_report(args):
    day = args.day or datetime.date.today().isoformat()
    d = os.path.join(args.logs, day)
    if not os.path.isdir(d):
        sys.exit(f"no records in {d}")
    files = sorted(f for f in os.listdir(d) if f.endswith(".json"))
    out = [f"# C4FM bench report {day}", ""]
    for name in files:
        with open(os.path.join(d, name)) as f:
            r = json.load(f)
        out.append(f"## {name}: {r['kind']} '{r['label']}' ({r['time']})")
        if r.get("note"):
            out.append(f"Note: {r['note']}")
        out.append("")
        if r.get("restarted"):
            out.append("**The radio restarted during this test: counters lost.**")
            out.append("")
        if r.get("radio"):
            out.append("Radio: " + ", ".join(f"{k}={v}" for k, v in r["radio"].items()))
            out.append("")
        for part in ("rx", "tx", "metrics"):
            if not r.get(part):
                continue
            out.append(f"| {part} | value |")
            out.append("|---|---|")
            for k, v in r[part].items():
                out.append(f"| {k} | {fmt(v, '') if isinstance(v, float) else v} |")
            out.append("")
        if r.get("knobs"):
            out.append("Knobs: " + ", ".join(f"{k}={v}" for k, v in r["knobs"].items()))
            out.append("")
        if r.get("fields"):
            out.append("```")
            for k, v in r["fields"].items():
                if k == "fich":
                    out.append("fich " + " ".join(f"{a}={b}" for a, b in (v or {}).items()))
                else:
                    out.append(f"{k:10s} {v or '-'}  {field_text(bytes.fromhex(v)) if v else ''}")
            out.append("```")
            out.append("")
    path = os.path.join(d, f"report-{day}.md")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print(path)


def main():
    ap = argparse.ArgumentParser(description="C4FM bench session over the debug link",
                                 epilog=__doc__.split("\n\n", 1)[1], formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-p", "--port", default=argparse.SUPPRESS, help="serial port of the cable, e.g. /dev/ttyUSB0")
    common.add_argument("--logs", default=argparse.SUPPRESS)
    ap.add_argument("-p", "--port", default=os.environ.get("RFOUNDRY_PORT"))
    ap.add_argument("--logs", default=DEFAULT_LOGS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    _add = sub.add_parser
    sub.add_parser = lambda *a, **k: _add(*a, parents=[common], **k)
    u = sub.add_parser("setup")
    u.add_argument("--qrg", metavar="MHZ", help="test frequency, e.g. 433.4250 (opens the VFO)")
    u.add_argument("--call", metavar="CALL", help="the callsign the radio should send, checked")
    u.add_argument("--power", default="L", help="L | M | H (the board's level of that name), lowest, highest or a "
                   "level number (default L)")
    st = sub.add_parser("status")
    st.add_argument("--all", action="store_true", help="every counter")
    w = sub.add_parser("watch")
    w.add_argument("--interval", type=float, default=1.0)
    p = sub.add_parser("begin")
    p.add_argument("label")
    p.add_argument("--note")
    p = sub.add_parser("end")
    p.add_argument("--note", help="what you heard: score 1-5 and a few words")
    f = sub.add_parser("fields")
    f.add_argument("--compare", metavar="FILE", help="a saved record (JSON) to compare with, field by field")
    f.add_argument("--save", metavar="LABEL", help="also save these fields as a record")
    sub.add_parser("knobs")
    s = sub.add_parser("set")
    s.add_argument("what", choices=["mic-gain", "nr", "agc", "tone", "level"])
    s.add_argument("value")
    r = sub.add_parser("report")
    r.add_argument("--day")
    args = ap.parse_args()

    if args.cmd == "report":
        cmd_report(args)
        return
    try:
        with rd.DebugLink(args.port) as dl:
            if True:
                {"begin": cmd_begin, "end": cmd_end, "setup": cmd_setup, "status": cmd_status, "watch": cmd_watch, "fields": cmd_fields, "knobs": cmd_knobs,
                 "set": cmd_set}[args.cmd](dl, args)
    except rd.DebugLinkError as e:
        sys.exit(f"debug link: {e}")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
