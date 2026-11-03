#!/usr/bin/env python3
"""Draw the README benchmark charts from bench/results/*.json.

Uses only the standard library. Takes the runs of the most recent machine and
commit in bench/results/, and writes light and dark SVGs to docs/img/:

  bench-typical-{light,dark}.svg  typical cost per operation (batched mean)
  bench-tail-{light,dark}.svg     p50 / p99 / p99.99 per operation

Usage: python3 scripts/plot_bench.py
"""
import glob
import json
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OPS = [("add (passive)", "Add"), ("cancel", "Cancel"),
       ("match (1 fill)", "Match (1 fill)"), ("modify (qty down)", "Modify")]

THEMES = {
    # Reference palette: chart chrome + blue ramp (validated: ordinal, both modes).
    "light": dict(surface="#fcfcfb", primary="#0b0b0b", secondary="#52514e", muted="#898781",
                  grid="#e1e0d9", axis="#c3c2b7", bar="#2a78d6",
                  ramp=["#86b6ef", "#2a78d6", "#104281"]),
    "dark": dict(surface="#1a1a19", primary="#f0efec", secondary="#c3c2b7", muted="#898781",
                 grid="#2c2c2a", axis="#383835", bar="#3987e5",
                 ramp=["#184f95", "#3987e5", "#9ec5f4"]),
}
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"


def load_runs():
    files = sorted(glob.glob(os.path.join(ROOT, "bench", "results", "*.json")))
    runs = []
    for f in files:
        with open(f) as fh:
            d = json.load(fh)
        runs.append((f, d))
    if not runs:
        sys.exit("no results in bench/results/; run ./scripts/bench.sh first")
    # Most recent machine + commit; all of its runs.
    latest = runs[-1][1]
    key = (latest["machine"]["model"], latest["git_commit"])
    chosen = [d for _, d in runs if (d["machine"]["model"], d["git_commit"]) == key]
    return chosen


def rounded_bar_h(x, y, w, h, r=4):
    """Horizontal bar from x (baseline) rightwards, rounded only at the data end."""
    r = min(r, w, h / 2)
    return (f"M{x:.1f},{y:.1f} H{x + w - r:.1f} Q{x + w:.1f},{y:.1f} {x + w:.1f},{y + r:.1f} "
            f"V{y + h - r:.1f} Q{x + w:.1f},{y + h:.1f} {x + w - r:.1f},{y + h:.1f} H{x:.1f} Z")


def rounded_bar_v(x, base, w, h, r=4):
    """Vertical bar rising from the baseline, rounded only at the data end (top)."""
    if h <= 0:
        return ""
    r = min(r, h, w / 2)
    top = base - h
    return (f"M{x:.1f},{base:.1f} V{top + r:.1f} Q{x:.1f},{top:.1f} {x + r:.1f},{top:.1f} "
            f"H{x + w - r:.1f} Q{x + w:.1f},{top:.1f} {x + w:.1f},{top + r:.1f} V{base:.1f} Z")


def text(x, y, s, fill, size=13, anchor="start", weight="normal"):
    s = s.replace("&", "&amp;").replace("<", "&lt;")
    return (f'<text x="{x:.1f}" y="{y:.1f}" fill="{fill}" font-size="{size}" '
            f'font-weight="{weight}" text-anchor="{anchor}" font-family="{FONT}">{s}</text>')


def typical_svg(runs, t, machine):
    W, H = 760, 292
    left, right, top = 150, 120, 74
    row_h, bar_h = 40, 22
    xmax = 60.0
    plot_w = W - left - right
    sx = lambda v: left + v / xmax * plot_w
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
           f'role="img" aria-label="Typical cost per order book operation in nanoseconds">',
           f'<rect width="{W}" height="{H}" rx="8" fill="{t["surface"]}"/>',
           text(20, 30, "Typical cost per order book operation (lower is better)", t["primary"], 16, weight="600"),
           text(20, 52, f"{machine}, one core, in process. Mean of {len(runs)} runs; range across runs in brackets.",
                t["secondary"], 12.5)]
    plot_bottom = top + row_h * len(OPS)
    for v in range(0, int(xmax) + 1, 10):
        x = sx(v)
        out.append(f'<line x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{plot_bottom}" stroke="{t["grid"]}" stroke-width="1"/>')
        out.append(text(x, plot_bottom + 18, f"{v}", t["muted"], 11.5, "middle"))
    out.append(text(left + plot_w / 2, plot_bottom + 36, "nanoseconds per operation", t["muted"], 11.5, "middle"))
    out.append(f'<line x1="{left}" y1="{top - 6}" x2="{left}" y2="{plot_bottom}" stroke="{t["axis"]}" stroke-width="1"/>')
    for i, (key, label) in enumerate(OPS):
        vals = [r["bench_book"]["results_ns"][key]["batched_per_op"] for r in runs]
        mean, lo, hi = statistics.mean(vals), min(vals), max(vals)
        y = top + i * row_h + (row_h - bar_h) / 2 - 4
        out.append(text(left - 12, y + bar_h / 2 + 4.5, label, t["secondary"], 13, "end"))
        out.append(f'<path d="{rounded_bar_h(left, y, sx(mean) - left, bar_h)}" fill="{t["bar"]}">'
                   f'<title>{label}: {mean:.1f} ns (runs: {lo:.1f} to {hi:.1f})</title></path>')
        out.append(text(sx(mean) + 8, y + bar_h / 2 + 4.5, f"{mean:.1f} ns", t["primary"], 13, weight="600"))
        out.append(text(sx(mean) + 64, y + bar_h / 2 + 4.5, f"({lo:.1f}–{hi:.1f})", t["muted"], 11.5))
    out.append("</svg>")
    return "\n".join(out)


def tail_svg(runs, t, machine, tick):
    W, H = 760, 344
    left, right, top = 56, 24, 108
    plot_h = 160
    ymax = 250.0
    pcts = [("p50", "p50"), ("p99", "p99"), ("p99_99", "p99.99")]
    group_w = (W - left - right) / len(OPS)
    bar_w, gap = 26, 2
    base = top + plot_h
    sy = lambda v: v / ymax * plot_h
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
           f'role="img" aria-label="Order book operation latency percentiles in nanoseconds">',
           f'<rect width="{W}" height="{H}" rx="8" fill="{t["surface"]}"/>',
           text(20, 30, "Tail latency per operation (worst of the runs, lower is better)", t["primary"], 16, weight="600"),
           text(20, 52, f"{machine}, one core, in process. Each operation timed on its own, "
                        f"{runs[0]['bench_book']['ops_per_type']:,} per type per run.", t["secondary"], 12.5)]
    # legend
    lx = 20
    for j, (_, name) in enumerate(pcts):
        out.append(f'<rect x="{lx}" y="64" width="12" height="12" rx="2" fill="{t["ramp"][j]}"/>')
        out.append(text(lx + 18, 74.5, name, t["secondary"], 12.5))
        lx += 86
    for v in range(0, int(ymax) + 1, 50):
        y = base - sy(v)
        out.append(f'<line x1="{left}" y1="{y:.1f}" x2="{W - right}" y2="{y:.1f}" stroke="{t["grid"]}" stroke-width="1"/>')
        out.append(text(left - 8, y + 4, f"{v}", t["muted"], 11.5, "end"))
    out.append(text(left - 8, top - 12, "ns", t["muted"], 11.5, "end"))
    out.append(f'<line x1="{left}" y1="{base}" x2="{W - right}" y2="{base}" stroke="{t["axis"]}" stroke-width="1"/>')
    for i, (key, label) in enumerate(OPS):
        gx = left + i * group_w
        total = len(pcts) * bar_w + (len(pcts) - 1) * gap
        x0 = gx + (group_w - total) / 2
        for j, (pk, pname) in enumerate(pcts):
            v = max(r["bench_book"]["results_ns"][key][pk] for r in runs)
            x = x0 + j * (bar_w + gap)
            if v > 0:
                out.append(f'<path d="{rounded_bar_v(x, base, bar_w, sy(v))}" fill="{t["ramp"][j]}">'
                           f'<title>{label} {pname}: {v} ns</title></path>')
            lab = f"{v}" if v > 0 else f"<{tick}"
            out.append(text(x + bar_w / 2, base - sy(v) - 6, lab, t["primary"], 11.5, "middle"))
        out.append(text(gx + group_w / 2, base + 20, label, t["secondary"], 13, "middle"))
    out.append(text(20, H - 34, f"The Apple M2 clock ticks every ~{tick} ns, so values move in ~{tick} ns steps; "
                                f"“<{tick}” means faster than one tick.", t["muted"], 11.5))
    worst = max(r["bench_book"]["results_ns"][k]["max"] for r in runs for k, _ in OPS)
    out.append(text(20, H - 16, f"Not shown: the single worst sample (up to {worst / 1000:.0f} µs) is the OS "
                                f"interrupting the thread; macOS cannot pin threads.", t["muted"], 11.5))
    out.append("</svg>")
    return "\n".join(out)


def main():
    runs = load_runs()
    m = runs[0]["machine"]
    cpu = m["cpu"].replace("Apple ", "Apple ")
    machine = f"{cpu} ({m['model']})"
    tick = max(r["bench_book"]["timer"]["resolution_ns"] for r in runs) + 1  # 41 ns step reported -> ~42
    out_dir = os.path.join(ROOT, "docs", "img")
    os.makedirs(out_dir, exist_ok=True)
    for mode, t in THEMES.items():
        with open(os.path.join(out_dir, f"bench-typical-{mode}.svg"), "w") as f:
            f.write(typical_svg(runs, t, machine))
        with open(os.path.join(out_dir, f"bench-tail-{mode}.svg"), "w") as f:
            f.write(tail_svg(runs, t, machine, tick))
    print(f"Charts written to docs/img/ from {len(runs)} runs of commit {runs[0]['git_commit']} on {machine}")


if __name__ == "__main__":
    main()
