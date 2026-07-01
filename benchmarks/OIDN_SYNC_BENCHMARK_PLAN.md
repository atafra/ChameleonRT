# OIDN Synchronization Mode Benchmark Plan (Vulkan + DXR)

Compare the performance of ChameleonRT's **OIDN interop / synchronization modes** on the
**Vulkan** and **DXR** backends at a fixed **1920x1080** resolution, using the Sponza test
scene, then produce **separate HTML analysis reports per backend**.

---

## 1. Tooling decision

Use ChameleonRT's own scripts, driven from the repository root:

- `scripts/capture_benchmarks.py` — runs `chameleonrt.exe` per variant, collects `benchmark.csv` / `benchmark.json` / `benchmark_meta.json`, and writes `scene_info.json`.
- `scripts/analyze_benchmarks.py` — parses the captured artifacts and generates the HTML report.

> The `rptr_benchmarks` MCP tooling is **not** used: it targets a different, Vulkan-only
> renderer (`rptr.exe`) that has no DXR backend and no OIDN interop modes, and uses a
> different (`--double-dash` / `.ini`) command syntax. It has been disabled by the user.

---

## 2. Environment

| Item | Value |
| --- | --- |
| Repo root (run all commands here) | `D:\Code\ChameleonRT_OIDN` |
| Executable | `build/vs/Release/chameleonrt.exe` |
| Backend plugins present | `crt_vulkan.dll`, `crt_dxr.dll` |
| OIDN runtime present next to exe | `OpenImageDenoise*.dll`, `sycl8.dll`, `ur_*.dll`, `SDL2.dll` |
| Scene | `./Assets/Sponza/sponza.obj` |
| Resolution | `1920 x 1080` (`-img 1920 1080`) |
| Frames per variant | `200` (implicit default via `-profiling`, `-profiling-fps 1`) |

**Prerequisites**

- Intel oneAPI compiler runtime on `PATH` for the OIDN SYCL device (e.g. prepend
  `C:\Program Files (x86)\Intel\oneAPI\compiler\latest\bin`) — required by the OIDN backend at runtime.
- Python packages for analysis: `pandas`, `plotly`, `chart_studio` (install on demand if an `ImportError` occurs, e.g. also `kaleido`).
- **Vulkan validation layers must be OFF for representative timings.** The app does not
  enable them itself (`make_instance` sets `enabledLayerCount = 0`); they are injected
  externally (e.g. a persisted Vulkan Configurator / vkconfig override), which adds large
  DebugPrintf overhead. Disable them for the run via the loader:
  `VK_LOADER_LAYERS_DISABLE = VK_LAYER_KHRONOS_validation`. (DXR is unaffected: its D3D12
  debug layer is `_DEBUG`-gated and this is a Release build.)

---

## 3. Configurations under test

Baseline = `host_blocking` (present on every device). GPU-interop modes are hardware-gated,
so they are **probed first** and only benchmarked if supported (an unsupported `-oidn-interop`
value makes `chameleonrt.exe` exit with code 1, which would abort a capture run).

**Vulkan candidates** (`get_supported_oidn_interop_modes`):

| Variant name | `-oidn-interop` | Notes |
| --- | --- | --- |
| `baseline_host_blocking` | `host_blocking` | Always supported. Baseline. |
| `timeline_semaphore` | `timeline_semaphore` | Requires timeline-semaphore support. |
| `binary_semaphore` | `binary_semaphore` | Requires external binary-semaphore support. |

> `timeline_semaphore_per_slot` is intentionally **excluded** (experimental, no measurable
> difference).

**DXR candidates:**

| Variant name | `-oidn-interop` | Notes |
| --- | --- | --- |
| `baseline_host_blocking` | `host_blocking` | Always supported. Baseline. |
| `device_async` | `device_async` | Requires device-async interop support. |

---

## 4. Output layout (single `benchmarks/` root)

```
benchmarks/
├─ OIDN_SYNC_BENCHMARK_PLAN.md      # this plan
├─ configs/
│  ├─ capture_oidn_vulkan.json
│  ├─ capture_oidn_dxr.json
│  ├─ report_oidn_vulkan.json
│  └─ report_oidn_dxr.json
├─ data/
│  ├─ oidn_vulkan/                  # per-variant benchmark.csv/json + scene_info.json
│  └─ oidn_dxr/
└─ reports/
   ├─ oidn_vulkan/                  # HTML analysis report (Vulkan)
   └─ oidn_dxr/                     # HTML analysis report (DXR)
```

---

## 5. Portability notes (design goal)

Configs are authored to be as reproducible as possible across machines/platforms:

- **Relative, forward-slash paths.** `crt_working_dir` is `"."` and all paths use `/`, which
  resolves correctly on Windows and POSIX **when scripts are run from the repository root**.
- **Isolated build-specific value.** Only `crt_executable_rel_path`
  (`build/vs/Release/chameleonrt.exe`) is build-config specific; adjust this one key per
  build tree / OS (e.g. drop `.exe`, change the config folder).
- **Backend caveat.** `capture_oidn_dxr.json` is inherently **Windows-only** (DXR). The
  Vulkan config is cross-platform.
- **Follow-up (future):** parameterize the executable path / OS-specific bits so the same
  config set runs unchanged on other platforms.

---

## 6. Config files

### `benchmarks/configs/capture_oidn_vulkan.json`
```json
{
	"title": "OIDN Sync Modes - Vulkan (Sponza, 1080p)",
	"crt_working_dir": ".",
	"crt_executable_rel_path": "build/vs/Release/chameleonrt.exe",
	"shared_benchmark_cmd_prefix": " vulkan ./Assets/Sponza/sponza.obj -img 1920 1080",
	"shared_benchmark_cmd_postfix": "",
	"benchmark_configs": [
		{ "name": "baseline_host_blocking", "desc": "Baseline: host-side blocking synchronization between the renderer and OIDN.", "benchmark_cmd": " -oidn-interop host_blocking" },
		{ "name": "timeline_semaphore",     "desc": "GPU-side synchronization via a shared timeline semaphore.",                    "benchmark_cmd": " -oidn-interop timeline_semaphore" },
		{ "name": "binary_semaphore",       "desc": "GPU-side synchronization via a shared binary semaphore.",                      "benchmark_cmd": " -oidn-interop binary_semaphore" }
	]
}
```

### `benchmarks/configs/capture_oidn_dxr.json`
```json
{
	"title": "OIDN Sync Modes - DXR (Sponza, 1080p)",
	"crt_working_dir": ".",
	"crt_executable_rel_path": "build/vs/Release/chameleonrt.exe",
	"shared_benchmark_cmd_prefix": " dxr ./Assets/Sponza/sponza.obj -img 1920 1080",
	"shared_benchmark_cmd_postfix": "",
	"benchmark_configs": [
		{ "name": "baseline_host_blocking", "desc": "Baseline: host-side blocking synchronization between the renderer and OIDN.", "benchmark_cmd": " -oidn-interop host_blocking" },
		{ "name": "device_async",           "desc": "GPU-side asynchronous denoising via device interop.",                          "benchmark_cmd": " -oidn-interop device_async" }
	]
}
```

### `benchmarks/configs/report_oidn_vulkan.json`
```json
{
	"title": "OIDN Synchronization Modes - Vulkan (Sponza, 1080p)",
	"description": "Comparison of OIDN interop/synchronization modes on the Vulkan backend at 1920x1080.",
	"ignore_frames": 20,
	"smoothing_window_size": 5,
	"generate_plots": [
		{ "perf_metric": "app_time_ms",     "alias": "Total Frame Time (ms)", "plot_type": "standard" },
		{ "perf_metric": "denoise_time_ms", "alias": "Denoise Time (ms)",     "plot_type": "standard" },
		{ "perf_metric": "render_time_ms",  "alias": "Render Time (ms)",      "plot_type": "standard" },
		{ "perf_metric": "app_time_ms",     "alias": "Total Frame Time vs host_blocking", "plot_type": "relative", "baseline": "baseline_host_blocking" }
	]
}
```

### `benchmarks/configs/report_oidn_dxr.json`
```json
{
	"title": "OIDN Synchronization Modes - DXR (Sponza, 1080p)",
	"description": "Comparison of OIDN interop/synchronization modes on the DXR backend at 1920x1080.",
	"ignore_frames": 20,
	"smoothing_window_size": 5,
	"generate_plots": [
		{ "perf_metric": "app_time_ms",     "alias": "Total Frame Time (ms)", "plot_type": "standard" },
		{ "perf_metric": "denoise_time_ms", "alias": "Denoise Time (ms)",     "plot_type": "standard" },
		{ "perf_metric": "render_time_ms",  "alias": "Render Time (ms)",      "plot_type": "standard" },
		{ "perf_metric": "app_time_ms",     "alias": "Total Frame Time vs host_blocking", "plot_type": "relative", "baseline": "baseline_host_blocking" }
	]
}
```

> Available CSV metrics (from `BenchmarkRecorder`): `frames_total`, `frames_accumulated`,
> `render_time_ms`, `app_time_ms`, `denoise_time_ms`, `tonemap_time_ms`, `rays_per_second`.

---

## 7. Execution phases

1. **Verify environment** — confirm `chameleonrt.exe`, Sponza, oneAPI runtime on `PATH`, and Python deps.
2. **Probe supported modes** — 1-frame run per candidate (`-benchmark-frames 1`); keep only modes that exit 0. Also confirms the DXR backend initializes.
3. **Author configs** — write the four JSON files above under `benchmarks/configs/` (trim any unsupported variants found in step 2).
4. **Capture Vulkan** — `capture_benchmarks.py capture_oidn_vulkan.json benchmarks/data/oidn_vulkan`.
5. **Capture DXR** — `capture_benchmarks.py capture_oidn_dxr.json benchmarks/data/oidn_dxr`.
6. **Analyze Vulkan** — `analyze_benchmarks.py report_oidn_vulkan.json benchmarks/data/oidn_vulkan benchmarks/reports/oidn_vulkan`.
7. **Analyze DXR** — `analyze_benchmarks.py report_oidn_dxr.json benchmarks/data/oidn_dxr benchmarks/reports/oidn_dxr`.
8. **Verify & summarize** — confirm CSV/JSON per variant, `scene_info.json`, and each backend's HTML report; summarize mean `app_time_ms` / `denoise_time_ms` and the relative deltas vs `host_blocking`.

---

## 8. Reproduction (run from repo root `D:\Code\ChameleonRT_OIDN`)

### One-shot script (recommended)

`benchmarks/run_oidn_benchmarks.ps1` reproduces the whole flow (prereq checks, oneAPI
`PATH`, Vulkan-validation suppression, capture + analysis for both backends) on a fresh
system. It resolves the repo root from its own location, so it can be run from anywhere.

```powershell
# From the repo root (PowerShell)
.\benchmarks\run_oidn_benchmarks.ps1

# Or double-click / call the .cmd launcher (bypasses execution policy, no PATH setup)
benchmarks\run_oidn_benchmarks.cmd
```

Useful switches:

```powershell
.\benchmarks\run_oidn_benchmarks.ps1 -Backends vulkan            # single backend
.\benchmarks\run_oidn_benchmarks.ps1 -InstallDeps                # pip install analysis deps if missing
.\benchmarks\run_oidn_benchmarks.ps1 -SkipCapture                # re-generate reports from existing data
.\benchmarks\run_oidn_benchmarks.ps1 -OneApiBin "D:\path\to\oneAPI\compiler\latest\bin"
```

> On a new machine, adjust `crt_executable_rel_path` in the capture configs (and/or pass
> `-OneApiBin`) to match that build tree; everything else is repo-relative.

### Manual commands (equivalent)

```powershell
# One-time: analysis dependencies
pip install pandas plotly chart_studio

# One-time per shell: OIDN SYCL runtime
$env:PATH = "C:\Program Files (x86)\Intel\oneAPI\compiler\latest\bin;$env:PATH"

# One-time per shell: disable externally-injected Vulkan validation layers so
# timings are representative (no effect on DXR).
$env:VK_LOADER_LAYERS_DISABLE = "VK_LAYER_KHRONOS_validation"

# Capture
python scripts\capture_benchmarks.py benchmarks\configs\capture_oidn_vulkan.json benchmarks\data\oidn_vulkan
python scripts\capture_benchmarks.py benchmarks\configs\capture_oidn_dxr.json    benchmarks\data\oidn_dxr

# Analyze (separate report per backend)
python scripts\analyze_benchmarks.py benchmarks\configs\report_oidn_vulkan.json benchmarks\data\oidn_vulkan benchmarks\reports\oidn_vulkan
python scripts\analyze_benchmarks.py benchmarks\configs\report_oidn_dxr.json    benchmarks\data\oidn_dxr    benchmarks\reports\oidn_dxr
```

---

## 9. Progress checklist

- [x] 1. Verify environment
- [x] 2. Probe supported OIDN modes (Vulkan + DXR) — all 5 variants supported, no fallback
- [x] 3. Author config files
- [x] 4. Capture Vulkan (validation layers disabled)
- [x] 5. Capture DXR
- [x] 6. Analyze -> Vulkan report
- [x] 7. Analyze -> DXR report
- [x] 8. Verify artifacts & summarize

---

## 10. Results

Results are **platform-specific** and therefore not stored in this document. They are
generated automatically as a standalone, aesthetic HTML summary after the per-backend
reports:

- **Summary:** `benchmarks/reports/oidn_summary.html`
- **Per-backend detail:** `benchmarks/reports/oidn_vulkan/benchmark_report.html`,
  `benchmarks/reports/oidn_dxr/benchmark_report.html`

The summary is produced by `scripts/summarize_benchmarks.py` (Python standard library
only, so it runs even without the analysis dependencies) and is invoked automatically by
`run_oidn_benchmarks.ps1`. It mirrors the previous static table layout: an intro line, a
per-backend section (with the `host_blocking` baseline `app_time_ms`) and a
`Variant / app_time_ms / vs baseline / denoise_time_ms / render_time_ms` table, followed
by data-driven Takeaways. Means are computed per variant after discarding the first
`ignore_frames` frames (from the report config), with the `baseline*` variant used as the
per-backend baseline for the "vs baseline" column.

To regenerate the summary on its own from existing capture data:

```powershell
python scripts\summarize_benchmarks.py --output benchmarks\reports\oidn_summary.html --ignore-frames 20 `
    --backend Vulkan benchmarks\data\oidn_vulkan `
    --backend DXR    benchmarks\data\oidn_dxr
```

> Absolute Vulkan numbers are only representative when the externally-injected validation
> layers are disabled (see Prerequisites); otherwise DebugPrintf overhead dominates.
