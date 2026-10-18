"""Turn a frame-time CSV (t_s,frame_ms) into one self-contained HTML page.

Usage: python -m tools.perf.plot_frames <csv> [-o out.html] [--title TEXT]
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from html import escape
from pathlib import Path

SLOW_MS = 33.3
VERY_SLOW_MS = 50.0
Y_CAP_MS = 100.0
MAX_POINTS = 4000
HIST_MAX_MS = 60

VIEW_W = 640
VIEW_H = 240
PAD_L, PAD_R, PAD_T, PAD_B = 44, 12, 12, 28


@dataclass(frozen=True)
class Frames:
    t: list[float]
    ms: list[float]


@dataclass(frozen=True)
class Summary:
    count: int
    duration_s: float
    mean_fps: float
    mean_ms: float
    median_ms: float
    p95_ms: float
    p99_ms: float
    max_ms: float
    over_33: int
    over_50: int


def read_frames(path: Path) -> Frames:
    t: list[float] = []
    ms: list[float] = []
    with path.open(newline="") as f:
        reader = csv.reader(f)
        next(reader, None)  # header
        for row in reader:
            if len(row) < 2:
                continue
            try:
                t_s, frame_ms = float(row[0]), float(row[1])
            except ValueError:
                continue  # a partly written last line
            t.append(t_s)
            ms.append(frame_ms)
    return Frames(t, ms)


def percentile(sorted_vals: list[float], q: float) -> float:
    """Nearest-rank percentile of an ascending list, q in 0..100."""
    if not sorted_vals:
        return 0.0
    rank = max(1, math.ceil(q / 100.0 * len(sorted_vals)))
    return sorted_vals[rank - 1]


def summarize(frames: Frames) -> Summary:
    n = len(frames.ms)
    if n == 0:
        return Summary(0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0, 0)
    ordered = sorted(frames.ms)
    duration = max(frames.t[-1], 1e-9)  # t is seconds since the first frame
    return Summary(
        count=n,
        duration_s=duration,
        mean_fps=n / duration,
        mean_ms=sum(frames.ms) / n,
        median_ms=percentile(ordered, 50),
        p95_ms=percentile(ordered, 95),
        p99_ms=percentile(ordered, 99),
        max_ms=ordered[-1],
        over_33=sum(1 for v in frames.ms if v > SLOW_MS),
        over_50=sum(1 for v in frames.ms if v > VERY_SLOW_MS),
    )


def fps_per_second(frames: Frames) -> list[int]:
    """Frame count in each whole second of t."""
    if not frames.t:
        return []
    counts = [0] * (int(frames.t[-1]) + 1)
    for t in frames.t:
        counts[max(0, int(t))] += 1
    return counts


def downsample_max(frames: Frames, limit: int = MAX_POINTS) -> Frames:
    """At most `limit` points; each bucket keeps its slowest frame."""
    n = len(frames.ms)
    if n <= limit:
        return frames
    t: list[float] = []
    ms: list[float] = []
    for b in range(limit):
        lo, hi = b * n // limit, (b + 1) * n // limit
        if hi <= lo:
            continue
        peak = max(range(lo, hi), key=lambda i: frames.ms[i])
        t.append(frames.t[peak])
        ms.append(frames.ms[peak])
    return Frames(t, ms)


def histogram(ms: list[float]) -> list[int]:
    """1 ms bins for 0..HIST_MAX_MS, then one overflow bin."""
    bins = [0] * (HIST_MAX_MS + 1)
    for v in ms:
        bins[min(int(v), HIST_MAX_MS)] += 1
    return bins


def _svg_open(label: str) -> str:
    return (
        f'<svg viewBox="0 0 {VIEW_W} {VIEW_H}" width="100%" role="img" '
        f'aria-label="{escape(label)}" preserveAspectRatio="xMidYMid meet">'
    )


def _frame_box() -> str:
    return (
        f'<rect x="{PAD_L}" y="{PAD_T}" width="{VIEW_W - PAD_L - PAD_R}" '
        f'height="{VIEW_H - PAD_T - PAD_B}" class="plot"/>'
    )


def _hline(y: float, label: str, cls: str = "grid") -> str:
    return (
        f'<line x1="{PAD_L}" y1="{y:.1f}" x2="{VIEW_W - PAD_R}" y2="{y:.1f}" class="{cls}"/>'
        f'<text x="{PAD_L - 4}" y="{y + 3:.1f}" text-anchor="end" class="tick">{escape(label)}</text>'
    )


def _x_ticks(x_max: float, unit: str) -> str:
    out: list[str] = []
    plot_w = VIEW_W - PAD_L - PAD_R
    for i in range(6):
        v = x_max * i / 5
        x = PAD_L + plot_w * i / 5
        out.append(
            f'<text x="{x:.1f}" y="{VIEW_H - 10}" text-anchor="middle" class="tick">'
            f"{v:.0f}{unit}</text>"
        )
    return "".join(out)


def fps_chart(counts: list[int]) -> str:
    plot_w = VIEW_W - PAD_L - PAD_R
    plot_h = VIEW_H - PAD_T - PAD_B
    y_max = max(75.0, float(max(counts, default=0)) * 1.05)

    def y_of(v: float) -> float:
        return PAD_T + plot_h * (1.0 - v / y_max)

    x_max = max(len(counts) - 1, 1)
    pts = " ".join(
        f"{PAD_L + plot_w * i / x_max:.1f},{y_of(float(c)):.1f}" for i, c in enumerate(counts)
    )
    parts = [_svg_open("Frames per second over time"), _frame_box()]
    parts.append(_hline(y_of(30), "30"))
    parts.append(_hline(y_of(60), "60"))
    parts.append(_hline(y_of(0), "0"))
    parts.append(f'<polyline points="{pts}" class="line"/>')
    parts.append(_x_ticks(float(x_max), "s"))
    parts.append("</svg>")
    return "".join(parts)


def frame_chart(frames: Frames) -> str:
    plot_w = VIEW_W - PAD_L - PAD_R
    plot_h = VIEW_H - PAD_T - PAD_B

    def y_of(v: float) -> float:
        return PAD_T + plot_h * (1.0 - min(v, Y_CAP_MS) / Y_CAP_MS)

    pts_src = downsample_max(frames)
    x_max = max(frames.t[-1], 1e-9) if frames.t else 1.0
    parts = [_svg_open("Frame time in milliseconds over time"), _frame_box()]
    for ref in (16.7, SLOW_MS):
        parts.append(_hline(y_of(ref), f"{ref:g}", "ref"))
    parts.append(_hline(y_of(Y_CAP_MS), f"{Y_CAP_MS:.0f}+"))
    pts: list[str] = []
    marks: list[str] = []
    for t, v in zip(pts_src.t, pts_src.ms):
        x = PAD_L + plot_w * t / x_max
        pts.append(f"{x:.1f},{y_of(v):.1f}")
        if v > Y_CAP_MS:
            marks.append(
                f'<polygon points="{x - 3:.1f},{PAD_T + 7} {x + 3:.1f},{PAD_T + 7} '
                f'{x:.1f},{PAD_T}" class="spike"/>'
            )
    parts.append(f'<polyline points="{" ".join(pts)}" class="line"/>')
    parts.extend(marks)
    parts.append(_x_ticks(x_max, "s"))
    parts.append("</svg>")
    return "".join(parts)


def histogram_chart(bins: list[int]) -> str:
    plot_w = VIEW_W - PAD_L - PAD_R
    plot_h = VIEW_H - PAD_T - PAD_B
    top = max(bins, default=0) or 1
    bar_w = plot_w / len(bins)
    parts = [_svg_open("Histogram of frame time in milliseconds"), _frame_box()]
    parts.append(_hline(PAD_T, str(top)))
    parts.append(_hline(PAD_T + plot_h, "0"))
    for i, c in enumerate(bins):
        if c == 0:
            continue
        h = plot_h * c / top
        x = PAD_L + bar_w * i
        cls = "bar slow" if i * 1.0 >= SLOW_MS else "bar"
        parts.append(
            f'<rect x="{x:.2f}" y="{PAD_T + plot_h - h:.2f}" width="{max(bar_w - 0.5, 0.5):.2f}" '
            f'height="{h:.2f}" class="{cls}"><title>{i}'
            f'{"+" if i == HIST_MAX_MS else ""} ms: {c}</title></rect>'
        )
    for i in range(0, HIST_MAX_MS + 1, 10):
        x = PAD_L + bar_w * i
        label = f"{i}ms" if i < HIST_MAX_MS else f"{i}+"
        parts.append(
            f'<text x="{x:.1f}" y="{VIEW_H - 10}" text-anchor="middle" class="tick">{label}</text>'
        )
    parts.append("</svg>")
    return "".join(parts)


CSS = """
:root {
  --bg: #ffffff; --fg: #1a1a1a; --muted: #5c5c5c; --card: #f4f4f6; --border: #d6d6dc;
  --line: #1565c0; --ref: #d98200; --slow: #c62828; --grid: #c8c8d0;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #121316; --fg: #ececec; --muted: #a0a0a8; --card: #1c1d22; --border: #35363d;
    --line: #64b5f6; --ref: #ffb74d; --slow: #ef5350; --grid: #3a3b42;
  }
}
:root[data-theme="dark"] {
  --bg: #121316; --fg: #ececec; --muted: #a0a0a8; --card: #1c1d22; --border: #35363d;
  --line: #64b5f6; --ref: #ffb74d; --slow: #ef5350; --grid: #3a3b42;
}
* { box-sizing: border-box; }
html, body { margin: 0; background: var(--bg); color: var(--fg); }
body { font-family: Arial, sans-serif; padding: 16px; max-width: 900px; margin: 0 auto;
  overflow-x: hidden; }
h1 { font-size: 1.4rem; margin: 0 0 12px; }
h2 { font-size: 1.05rem; margin: 24px 0 8px; }
table { border-collapse: collapse; width: 100%; background: var(--card); }
th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid var(--border); }
td.num { font-family: ui-monospace, Menlo, monospace; text-align: right; }
svg { display: block; width: 100%; height: auto; background: var(--card);
  border: 1px solid var(--border); border-radius: 6px; }
.plot { fill: none; stroke: var(--border); }
.grid { stroke: var(--grid); stroke-width: 1; }
.ref { stroke: var(--ref); stroke-width: 1; stroke-dasharray: 4 3; }
.line { fill: none; stroke: var(--line); stroke-width: 1; stroke-linejoin: round; }
.bar { fill: var(--line); }
.bar.slow { fill: var(--slow); }
.spike { fill: var(--slow); }
.tick { fill: var(--muted); font-size: 10px; font-family: Arial, sans-serif; }
.note { color: var(--muted); font-size: 0.85rem; margin: 6px 0 0; }
"""


def _row(label: str, value: str) -> str:
    return f'<tr><th scope="row">{escape(label)}</th><td class="num">{escape(value)}</td></tr>'


def render(frames: Frames, title: str) -> str:
    s = summarize(frames)

    def pct(n: int) -> str:
        return f"{100.0 * n / s.count:.2f}%" if s.count else "0.00%"

    rows = [
        _row("Frames", str(s.count)),
        _row("Duration", f"{s.duration_s:.1f} s"),
        _row("Mean FPS", f"{s.mean_fps:.0f}"),
        _row("Mean frame time", f"{s.mean_ms:.1f} ms"),
        _row("Median frame time", f"{s.median_ms:.1f} ms"),
        _row("95th percentile", f"{s.p95_ms:.1f} ms"),
        _row("99th percentile", f"{s.p99_ms:.1f} ms"),
        _row("Max frame time", f"{s.max_ms:.1f} ms"),
        _row(f"Frames over {SLOW_MS} ms", f"{s.over_33} ({pct(s.over_33)})"),
        _row(f"Frames over {VERY_SLOW_MS:.0f} ms", f"{s.over_50} ({pct(s.over_50)})"),
    ]
    heading = escape(title)
    return (
        "<!DOCTYPE html>\n"
        '<html lang="en">\n<head>\n'
        '<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        "<title>Frame Times</title>\n"
        f"<style>{CSS}</style>\n</head>\n<body>\n"
        f"<h1>{heading}</h1>\n"
        f"<table>{''.join(rows)}</table>\n"
        "<h2>Frames per second</h2>\n"
        f"{fps_chart(fps_per_second(frames))}\n"
        "<h2>Frame time</h2>\n"
        f"{frame_chart(frames)}\n"
        f'<p class="note">Dashed lines mark 16.7 ms and {SLOW_MS} ms. Values above '
        f"{Y_CAP_MS:.0f} ms sit at the top with a marker. Long runs show the slowest frame "
        f"in each group of frames.</p>\n"
        "<h2>Frame time distribution</h2>\n"
        f"{histogram_chart(histogram(frames.ms))}\n"
        "</body>\n</html>\n"
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Plot a frame-time CSV as a self-contained HTML page.")
    ap.add_argument("csv", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=None)
    ap.add_argument("--title", default="Frame Times")
    args = ap.parse_args(argv)

    frames = read_frames(args.csv)
    if not frames.ms:
        print(f"plot_frames: no frames in {args.csv}", file=sys.stderr)
        return 1
    out: Path = args.output or args.csv.with_suffix(".html")
    out.write_text(render(frames, args.title), encoding="utf-8")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
