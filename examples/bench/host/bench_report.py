#!/usr/bin/env python3
"""Turn `bench` logs (@BENCH lines captured from the device console, and optionally a host_bench log) into
Markdown tables.

    python3 -I host/bench_report.py device.log [--host host.log] [--md out.md]

Lines are "@BENCH\\tk=v..." anywhere in a line (console noise around them is skipped); lines of the form
"<ms>\\tk=v..." (a timestamped TSV of the same fields) are accepted too. Kernel lines are keyed by every field
except the metric fields; with --host the table gets the host median and device/host ratio (by ns).
"""
import argparse
import sys
from collections import OrderedDict

METRIC = {"unit", "ops", "reps", "cyc_min", "cyc_med", "cyc_max", "ns_min", "ns_med", "ns_max", "spread_pct",
          "mb_s", "chk"}


def parse(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            i = line.find("@BENCH\t")
            if i >= 0:
                body = line[i + 7:]
            else:
                parts = line.split("\t", 1)  # BENCH.TSV: <ms>\t<fields>
                if len(parts) != 2 or not parts[0].isdigit():
                    continue
                body = parts[1]
            d = OrderedDict()
            for kv in body.split("\t"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    d[k] = v
            if d:
                rows.append(d)
    return rows


def key(d):
    return tuple((k, v) for k, v in d.items() if k not in METRIC and k not in ("ok", "blocks", "set", "chunk"))


def fnum(v, nd=2):
    try:
        x = float(v)
    except (TypeError, ValueError):
        return v or ""
    if x == 0:
        return "0"
    if abs(x) >= 100:
        return f"{x:,.0f}"
    if abs(x) >= 10:
        return f"{x:.1f}"
    return f"{x:.{nd}f}"


def label(d):
    skip = {"suite", "name"} | METRIC | {"ok"}
    extra = " ".join(f"{k}={v}" for k, v in d.items() if k not in skip and k != "note")
    return d.get("name", ""), extra


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("--host")
    ap.add_argument("--md")
    a = ap.parse_args()
    dev = parse(a.device)
    host = {key(r): r for r in parse(a.host)} if a.host else {}
    out = []
    envs = [r for r in dev if r.get("phase") == "env"]
    if envs:
        out.append("## Environment\n")
        e = envs[0]
        out.append("| key | value |\n|---|---|")
        for k, v in e.items():
            if k not in ("phase", "suite"):
                out.append(f"| {k} | {v} |")
        out.append("")
    suites = OrderedDict()
    for r in dev:
        if "suite" in r and r.get("phase") is None:
            suites.setdefault(r["suite"], []).append(r)
    bad = [r for r in dev if r.get("ok") == "0" or r.get("name") == "error"]
    for s, rs in suites.items():
        out.append(f"## {s}\n")
        kern = [r for r in rs if "unit" in r]
        other = [r for r in rs if "unit" not in r]
        if kern:
            cyc = "cyc_med" in kern[0]
            hdr = ["name", "params", "unit", "cyc min" if cyc else "ns min", "cyc med" if cyc else "ns med",
                   "ns med", "spread %", "MB/s"]
            if host:
                hdr += ["host ns med", "dev/host"]
            out.append("| " + " | ".join(hdr) + " |")
            out.append("|" + "---|" * len(hdr))
            for r in kern:
                n, ex = label(r)
                p = "cyc" if cyc else "ns"
                row = [n, ex, r["unit"], fnum(r.get(p + "_min")), fnum(r.get(p + "_med")), fnum(r.get("ns_med")),
                       fnum(r.get("spread_pct"), 1), fnum(r.get("mb_s"))]
                if host:
                    h = host.get(key(r))
                    if h and float(h.get("ns_med", 0) or 0) > 0:
                        row += [fnum(h["ns_med"], 3), fnum(float(r["ns_med"]) / float(h["ns_med"]), 1)]
                    else:
                        row += ["", ""]
                out.append("| " + " | ".join(row) + " |")
            out.append("")
        if other:
            keys = []
            for r in other:
                for k in r:
                    if k not in keys and k != "suite":
                        keys.append(k)
            out.append("| " + " | ".join(keys) + " |")
            out.append("|" + "---|" * len(keys))
            for r in other:
                out.append("| " + " | ".join(fnum(r.get(k, ""), 2) if k not in ("name", "path", "file", "open",
                                                                                   "after", "cls", "buf") else
                                               r.get(k, "") for k in keys) + " |")
            out.append("")
    done = [r for r in dev if r.get("phase") in ("suite_done", "done")]
    if done:
        out.append("## Run\n")
        out.append("| suite | bad | ms |\n|---|---|---|")
        for r in done:
            out.append(f"| {r.get('suite', 'all')} | {r.get('bad')} | {r.get('ms')} |")
        out.append("")
    out.append(f"Self-check failures / errors: {len(bad)}")
    for r in bad:
        out.append("- " + " ".join(f"{k}={v}" for k, v in r.items()))
    text = "\n".join(out) + "\n"
    if a.md:
        with open(a.md, "w", encoding="utf-8") as f:
            f.write(text)
    sys.stdout.write(text)


if __name__ == "__main__":
    main()
