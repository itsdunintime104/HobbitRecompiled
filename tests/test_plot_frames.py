from __future__ import annotations

import math
from pathlib import Path

from tools.perf import plot_frames


def _write_csv(path: Path) -> list[float]:
    ms = [16.7] * 3000
    for i in (500, 1200, 2000):
        ms[i] = 50.0
    ms[2500] = 200.0
    t = 0.0
    lines = ["t_s,frame_ms"]
    for v in ms:
        t += v / 1000.0
        lines.append(f"{t:.4f},{v:.3f}")
    path.write_text("\n".join(lines) + "\n")
    return ms


def test_plot_frames_html(tmp_path: Path) -> None:
    csv_path = tmp_path / "frames.csv"
    ms = _write_csv(csv_path)
    assert plot_frames.main([str(csv_path)]) == 0
    html = (tmp_path / "frames.html").read_text()

    assert "<svg" in html
    assert "<title>Frame Times</title>" in html
    assert "<script src=" not in html
    assert "<script" not in html
    assert "http" not in html

    ordered = sorted(ms)
    duration = sum(ms) / 1000.0
    mean_fps = len(ms) / duration
    p99 = ordered[math.ceil(0.99 * len(ms)) - 1]
    assert f"{len(ms)}</td>" in html
    assert f"{mean_fps:.0f}</td>" in html
    assert f"{p99:.1f} ms" in html
    assert f"{sum(1 for v in ms if v > 33.3)} (" in html  # 4 frames over 33.3 ms
    assert "200.0 ms" in html
