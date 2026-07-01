#!/usr/bin/env python3
# Generates an aesthetic, self-contained HTML summary of ChameleonRT OIDN
# synchronization-mode benchmarks. It mirrors "Section 10" of
# benchmarks/OIDN_SYNC_BENCHMARK_PLAN.md (per-backend mean-time tables + takeaways),
# but as a script-generated artifact so the platform-specific numbers never live in
# the committed plan.
#
# The summary is intentionally dependency-free (Python standard library only), so it
# runs even when the analysis dependencies (pandas/plotly) are not installed.
#
# Usage:
#   python summarize_benchmarks.py --output <out.html> [--ignore-frames N] [--title T]
#       --backend <Label> <data_dir> [--backend <Label> <data_dir> ...]
#
# Each <data_dir> is a capture output directory containing per-variant subdirectories
# with benchmark.csv / benchmark.json / benchmark_meta.json (as produced by
# capture_benchmarks.py). The variant whose directory name starts with "baseline" is
# used as the per-backend baseline for the relative ("vs baseline") column.

import argparse
import csv
import html
import json
import os
import statistics
import sys
from datetime import datetime
from pathlib import Path

# Columns summarized from each benchmark.csv. "app_time_ms" drives the relative
# ("vs baseline") column and the in-cell bars.
METRIC_KEYS = ["app_time_ms", "denoise_time_ms", "render_time_ms"]
METRIC_LABELS = {
    "app_time_ms": "app_time_ms",
    "denoise_time_ms": "denoise_time_ms",
    "render_time_ms": "render_time_ms",
}
BASELINE_PREFIX = "baseline"

CSS = """
:root {
  --bg: #0f1420; --panel: #171d2b; --panel-2: #1e2537; --line: #2a3346;
  --text: #e6e9f0; --muted: #97a2b8; --accent: #5b8cff;
  --faster: #37d39b; --slower: #ff6b6b; --baseline: #f4c04e;
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 2.5rem 1rem; background: var(--bg); color: var(--text);
  font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
  line-height: 1.5;
}
.container { max-width: 960px; margin: 0 auto; }
header.report { margin-bottom: 1.75rem; }
h1 { font-size: 1.65rem; margin: 0 0 .35rem; letter-spacing: .2px; }
.subtitle { color: var(--muted); font-size: .95rem; margin: 0; }
.intro { color: var(--muted); font-size: .92rem; margin: 1rem 0 1.5rem; }
.meta {
  display: flex; flex-wrap: wrap; gap: .5rem .75rem; margin-top: 1rem;
}
.chip {
  background: var(--panel-2); border: 1px solid var(--line); border-radius: 999px;
  padding: .3rem .7rem; font-size: .8rem; color: var(--muted);
}
.chip b { color: var(--text); font-weight: 600; }
section.backend {
  background: var(--panel); border: 1px solid var(--line); border-radius: 14px;
  padding: 1.25rem 1.35rem 1.4rem; margin-bottom: 1.4rem;
  box-shadow: 0 6px 24px rgba(0,0,0,.25);
}
section.backend h2 { font-size: 1.2rem; margin: 0 0 .15rem; }
section.backend h2 .baseval { color: var(--muted); font-weight: 500; font-size: .95rem; }
table { width: 100%; border-collapse: collapse; margin-top: .9rem; font-size: .92rem; }
th, td { padding: .55rem .65rem; text-align: right; border-bottom: 1px solid var(--line); }
th:first-child, td:first-child { text-align: left; }
thead th { color: var(--muted); font-weight: 600; font-size: .78rem;
  text-transform: uppercase; letter-spacing: .5px; border-bottom: 1px solid var(--line); }
tbody tr:last-child td { border-bottom: none; }
tbody tr.baseline { background: rgba(244,192,78,.07); }
.variant { display: flex; align-items: center; gap: .5rem; }
.variant code { background: var(--panel-2); border: 1px solid var(--line);
  padding: .1rem .4rem; border-radius: 6px; font-size: .86rem; }
.badge { font-size: .68rem; font-weight: 700; letter-spacing: .4px; padding: .12rem .45rem;
  border-radius: 999px; text-transform: uppercase; }
.badge.base { color: var(--baseline); border: 1px solid rgba(244,192,78,.4); }
.badge.best { color: var(--faster); border: 1px solid rgba(55,211,155,.4); }
td.num { font-variant-numeric: tabular-nums; position: relative; }
td.app { min-width: 130px; }
.bar { position: absolute; left: 0; top: 0; bottom: 0; background: rgba(91,140,255,.14);
  border-right: 2px solid rgba(91,140,255,.5); border-radius: 0 4px 4px 0; z-index: 0; }
td.app span { position: relative; z-index: 1; }
.delta.faster { color: var(--faster); font-weight: 600; }
.delta.slower { color: var(--slower); font-weight: 600; }
.delta.na { color: var(--muted); }
.takeaways { background: var(--panel); border: 1px solid var(--line); border-radius: 14px;
  padding: 1.1rem 1.35rem 1.25rem; margin-bottom: 1.4rem; }
.takeaways h2 { font-size: 1.1rem; margin: 0 0 .6rem; }
.takeaways ul { margin: 0; padding-left: 1.15rem; }
.takeaways li { margin: .3rem 0; }
.takeaways code, .links code { background: var(--panel-2); border: 1px solid var(--line);
  padding: .05rem .35rem; border-radius: 6px; font-size: .86rem; }
.links { font-size: .9rem; color: var(--muted); }
.links a { color: var(--accent); text-decoration: none; }
.links a:hover { text-decoration: underline; }
footer.report { color: var(--muted); font-size: .82rem; margin-top: 1.5rem;
  border-top: 1px solid var(--line); padding-top: 1rem; }
.empty { color: var(--muted); font-style: italic; margin-top: .75rem; }
"""


def read_csv_means(csv_path, ignore_frames):
    """Return (means_by_metric, total_rows, used_rows) for one benchmark.csv."""
    rows = []
    with open(csv_path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    total = len(rows)
    used = rows[ignore_frames:] if ignore_frames > 0 else rows
    means = {}
    for key in METRIC_KEYS:
        vals = []
        for r in used:
            v = r.get(key)
            if v in (None, ""):
                continue
            try:
                vals.append(float(v))
            except ValueError:
                continue
        means[key] = statistics.fmean(vals) if vals else None
    return means, total, len(used)


def load_json(path):
    try:
        return json.loads(Path(path).read_text())
    except Exception:
        return {}


def load_variant(dir_path, ignore_frames):
    csv_path = dir_path / "benchmark.csv"
    if not csv_path.is_file():
        return None
    means, total, used = read_csv_means(csv_path, ignore_frames)
    meta = load_json(dir_path / "benchmark_meta.json")
    return {
        "name": dir_path.name,
        "means": means,
        "total_frames": total,
        "used_frames": used,
        "desc": meta.get("desc", ""),
        "benchmark_json": load_json(dir_path / "benchmark.json"),
    }


def load_backend(label, data_dir, ignore_frames):
    data_dir = Path(data_dir)
    variants = []
    if data_dir.is_dir():
        for child in sorted(data_dir.iterdir()):
            if child.is_dir() and (child / "benchmark.csv").is_file():
                v = load_variant(child, ignore_frames)
                if v:
                    variants.append(v)
    baseline = next(
        (v for v in variants if v["name"].lower().startswith(BASELINE_PREFIX)), None
    )
    if baseline is None and variants:
        baseline = variants[0]
    others = sorted((v for v in variants if v is not baseline), key=lambda v: v["name"])
    ordered = ([baseline] if baseline else []) + others
    return {"label": label, "data_dir": data_dir, "variants": ordered, "baseline": baseline}


def extract_env(backend):
    base = backend.get("baseline")
    bj = base["benchmark_json"] if base else {}
    launch = bj.get("launch", {})
    system = bj.get("system", {})
    res = launch.get("render_res") or launch.get("display_res")
    res_str = None
    if isinstance(res, list) and len(res) == 2:
        res_str = f"{res[0]}x{res[1]}"
    scene = None
    for tok in launch.get("cmdline", []):
        if str(tok).lower().endswith((".obj", ".gltf", ".glb")):
            scene = Path(str(tok)).stem
            break
    frames = base["total_frames"] if base else None
    return res_str, scene, system, frames


def delta_pct(variant_app, baseline_app):
    if variant_app is None or not baseline_app:
        return None
    return (variant_app / baseline_app - 1.0) * 100.0


def fmt_ms(v):
    return "&mdash;" if v is None else f"{v:.2f}"


def derive_report_link(data_dir, output_dir):
    """Best-effort link to the matching analyze_benchmarks.py report, using the
    repo's data/<x> -> reports/<x> convention. Returns a relative URL or None."""
    p = Path(data_dir).resolve()
    parts = list(p.parts)
    if "data" in parts:
        idx = len(parts) - 1 - parts[::-1].index("data")
        parts[idx] = "reports"
        candidate = Path(*parts) / "benchmark_report.html"
        if candidate.is_file():
            return os.path.relpath(candidate, output_dir).replace(os.sep, "/")
    return None


def fastest_variant(backend):
    base = backend.get("baseline")
    cands = [
        v for v in backend["variants"]
        if v is not base and v["means"].get("app_time_ms") is not None
    ]
    if not cands:
        return None
    return min(cands, key=lambda v: v["means"]["app_time_ms"])


def render_backend_section(backend, output_dir):
    label = html.escape(backend["label"])
    base = backend.get("baseline")
    variants = backend["variants"]
    if not variants:
        return (
            f'<section class="backend"><h2>{label}</h2>'
            f'<p class="empty">No benchmark data found in '
            f'{html.escape(str(backend["data_dir"]))}.</p></section>'
        )

    base_app = base["means"].get("app_time_ms") if base else None
    baseval = (
        f' <span class="baseval">(baseline <code>app_time_ms</code> = {base_app:.2f} ms)</span>'
        if base_app is not None else ""
    )

    app_vals = [v["means"].get("app_time_ms") for v in variants
                if v["means"].get("app_time_ms") is not None]
    max_app = max(app_vals) if app_vals else None
    fastest = fastest_variant(backend)

    rows = []
    for v in variants:
        is_base = v is base
        app = v["means"].get("app_time_ms")
        d = None if is_base else delta_pct(app, base_app)
        if is_base:
            delta_html = '<span class="delta na">&mdash;</span>'
        elif d is None:
            delta_html = '<span class="delta na">&mdash;</span>'
        else:
            cls = "faster" if d < 0 else "slower"
            delta_html = f'<span class="delta {cls}">{d:+.1f}%</span>'

        badge = ""
        if is_base:
            badge = '<span class="badge base">baseline</span>'
        elif fastest is not None and v is fastest:
            badge = '<span class="badge best">fastest</span>'

        bar = ""
        if app is not None and max_app:
            pct = max(4.0, app / max_app * 100.0)
            bar = f'<div class="bar" style="width:{pct:.1f}%"></div>'

        rows.append(
            f'<tr class="{ "baseline" if is_base else "" }">'
            f'<td><div class="variant"><code>{html.escape(v["name"])}</code>{badge}</div></td>'
            f'<td class="num app">{bar}<span>{fmt_ms(app)}</span></td>'
            f'<td class="num">{delta_html}</td>'
            f'<td class="num">{fmt_ms(v["means"].get("denoise_time_ms"))}</td>'
            f'<td class="num">{fmt_ms(v["means"].get("render_time_ms"))}</td>'
            f'</tr>'
        )

    link = derive_report_link(backend["data_dir"], output_dir)
    link_html = (
        f'<p class="links">Detailed report: <a href="{html.escape(link)}">{html.escape(link)}</a></p>'
        if link else ""
    )

    return (
        f'<section class="backend"><h2>{label}{baseval}</h2>'
        f'<table><thead><tr>'
        f'<th>Variant</th><th>app_time_ms</th><th>vs baseline</th>'
        f'<th>denoise_time_ms</th><th>render_time_ms</th>'
        f'</tr></thead><tbody>{"".join(rows)}</tbody></table>'
        f'{link_html}</section>'
    )


def build_takeaways(backends):
    bullets = []
    deltas = []
    for b in backends:
        base = b.get("baseline")
        if not base:
            continue
        base_app = base["means"].get("app_time_ms")
        fastest = fastest_variant(b)
        if fastest is None or not base_app:
            continue
        d = delta_pct(fastest["means"]["app_time_ms"], base_app)
        deltas.append(d)
        dn_base = base["means"].get("denoise_time_ms")
        dn_fast = fastest["means"].get("denoise_time_ms")
        dn_txt = ""
        if dn_base and dn_fast is not None:
            dn_txt = f", denoise {(dn_fast / dn_base - 1.0) * 100.0:+.1f}%"
        bullets.append(
            f'<strong>{html.escape(b["label"])}</strong>: fastest mode is '
            f'<code>{html.escape(fastest["name"])}</code> at '
            f'{fastest["means"]["app_time_ms"]:.2f} ms ({d:+.1f}% vs '
            f'<code>{html.escape(base["name"])}</code>{dn_txt}).'
        )
    if bullets and deltas and all(d is not None and d < 0 for d in deltas):
        bullets.insert(
            0,
            "GPU-side interop modes reduce total frame time versus host-blocking on "
            "every tested backend by cutting the denoiser stall from the host round-trip.",
        )
    if not bullets:
        return ""
    items = "".join(f"<li>{b}</li>" for b in bullets)
    return f'<section class="takeaways"><h2>Takeaways</h2><ul>{items}</ul></section>'


def generate_html(backends, args, output_dir):
    scene = res = system = frames = None
    for b in backends:
        r, s, sysinfo, fr = extract_env(b)
        scene = scene or (args.scene or s)
        res = res or r
        frames = frames or fr
        if system is None and sysinfo:
            system = sysinfo

    header_bits = [x for x in (scene, res,
                               f"{frames} frames" if frames else None,
                               f"first {args.ignore_frames} ignored" if args.ignore_frames else None)
                   if x]
    default_title = "Results" + (f" ({', '.join(header_bits)})" if header_bits else "")
    title = args.title or default_title

    chips = []
    if system:
        if system.get("cpu"):
            chips.append(f'<span class="chip"><b>CPU</b> {html.escape(str(system["cpu"]))}</span>')
        if system.get("gpu"):
            chips.append(f'<span class="chip"><b>GPU</b> {html.escape(str(system["gpu"]))}</span>')
        gpu_name = str(system.get("gpu_name", "")).strip()
        if gpu_name:
            chips.append(f'<span class="chip"><b>GPU Name</b> {html.escape(gpu_name)}</span>')
        gpu_driver_version = str(system.get("gpu_driver_version", "")).strip()
        if gpu_driver_version:
            chips.append(f'<span class="chip"><b>GPU Driver</b> {html.escape(gpu_driver_version)}</span>')
    chips.append(
        f'<span class="chip"><b>Generated</b> '
        f'{datetime.now().strftime("%Y-%m-%d %H:%M")}</span>'
    )

    sections = "".join(render_backend_section(b, output_dir) for b in backends)
    takeaways = build_takeaways(backends)

    intro = (
        "Mean per-frame times; percentage is total frame time "
        "(<code>app_time_ms</code>) relative to the <code>host_blocking</code> "
        "baseline of the same backend (negative = faster)."
    )

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>{CSS}</style>
</head>
<body>
<div class="container">
<header class="report">
  <h1>{html.escape(title)}</h1>
  <p class="subtitle">OIDN synchronization-mode benchmark summary</p>
  <div class="meta">{"".join(chips)}</div>
</header>
<p class="intro">{intro}</p>
{takeaways}
{sections}
<footer class="report">
  Auto-generated by <code>scripts/summarize_benchmarks.py</code>. Means computed over
  each variant's frames after discarding the first {args.ignore_frames}. This report is
  platform-specific and regenerated on every benchmark run.
</footer>
</div>
</body>
</html>
"""


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Generate an HTML summary of OIDN sync-mode benchmark results."
    )
    parser.add_argument("--output", required=True, help="Path to the HTML file to write.")
    parser.add_argument(
        "--backend", nargs=2, action="append", dest="backends",
        metavar=("LABEL", "DATA_DIR"), required=True,
        help="Backend display label and its capture data directory. Repeatable.",
    )
    parser.add_argument("--ignore-frames", type=int, default=0,
                        help="Number of leading frames to discard per variant (default: 0).")
    parser.add_argument("--title", default=None, help="Override the report title.")
    parser.add_argument("--scene", default=None, help="Override the detected scene name.")
    args = parser.parse_args(argv)

    backends = [load_backend(label, data_dir, args.ignore_frames)
                for label, data_dir in args.backends]

    if not any(b["variants"] for b in backends):
        print("summarize_benchmarks: no benchmark data found in any --backend directory.",
              file=sys.stderr)
        return 1

    output_path = Path(args.output).resolve()
    output_dir = output_path.parent
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path.write_text(generate_html(backends, args, output_dir), encoding="utf-8")
    print(f"Summary written to {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
