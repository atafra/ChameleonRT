# ChameleonRT Benchmarking Scripts

Local, ChameleonRT-specific copies of the RPTR benchmark automation scripts. The
upstream RPTR originals (in the separate `rptr` tree) are intentionally left
untouched; these copies are adapted to drive `chameleonrt.exe` and to consume the
benchmark artifacts ChameleonRT emits via the `--profiling` option.

## Files

| File | Purpose |
| --- | --- |
| `capture_benchmarks.py` | Runs `chameleonrt.exe` for each configured variant and collects `benchmark.csv` / `benchmark.json` / `scene_info.json`. |
| `analyze_benchmarks.py` | Parses the captured artifacts and generates an HTML report with Plotly charts. |
| `benchmark_config.json` | Example capture configuration (Sponza + DXR). |
| `report_config.json` | Example report configuration (which metrics to plot). |

## Generated artifacts

ChameleonRT's `--profiling <base>` option writes:

* `<base>.csv` with the per-frame columns
  `frames_total,frames_accumulated,render_time_ms,app_time_ms,denoise_time_ms,tonemap_time_ms,rays_per_second`.
* `<base>.json` with `system` (cpu/gpu/display), `app` (backend) and `launch`
  (display/render resolution and command line) blocks.

`--scene-report <base>` writes `<base>.json` with the scene statistics under an
`info` object.

## Requirements

* Python 3 with `pandas`, `plotly`, and `chart_studio` installed.
* When ChameleonRT is built with OIDN, the Intel oneAPI runtime must be on
  `PATH` so the denoiser's dependencies (`libmmd.dll`, etc.) resolve, e.g.:

  ```powershell
  $env:PATH = "C:\Program Files (x86)\Intel\oneAPI\compiler\latest\bin;" + $env:PATH
  ```

## Usage

1. Edit `benchmark_config.json` to point `crt_working_dir` /
   `crt_executable_rel_path` at your build, and set the backend, scene, and any
   extra options in `shared_benchmark_cmd_prefix` / `benchmark_configs`.

2. Capture benchmarks (writes into `<BENCHMARK_DIR>`):

   ```powershell
   python capture_benchmarks.py benchmark_config.json my_benchmarks
   ```

3. Generate the report (writes into `<REPORT_DIR>`):

   ```powershell
   python analyze_benchmarks.py report_config.json my_benchmarks my_report
   ```

   Open `my_report/benchmark_report.html` to view the results.

## Configuration notes

### `benchmark_config.json`

* `crt_working_dir` — directory the executable is launched from (also where the
  intermediate `benchmark_script.csv` / `.json` are produced before being moved).
* `crt_executable_rel_path` — path to `chameleonrt.exe` relative to
  `crt_working_dir`.
* `shared_benchmark_cmd_prefix` — command-line text inserted before each
  variant's command (typically `<backend> <scene>`). Note the leading space.
* `shared_benchmark_cmd_postfix` — command-line text appended after each variant.
* `benchmark_configs` — one entry per variant; `benchmark_cmd` adds
  variant-specific options and `desc` is shown in the report.

The capture script always appends `--profiling benchmark_script --profiling-fps 1`
and `--scene-report <dir>/scene_info`.

### `report_config.json`

* `ignore_frames` — leading frames dropped from each CSV before plotting.
* `smoothing_window_size` — exponential-moving-average span for the line charts.
* `generate_plots` — list of charts. Each entry's `perf_metric` must match a CSV
  column name. `plot_type` is `standard` (absolute values) or `relative` (ratio
  to a named `baseline` variant).
