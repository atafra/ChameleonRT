# OIDN Benchmark Watchdog and Failure Reporting Plan

## Problem

Some GPU driver versions hang ChameleonRT when running specific asynchronous OIDN SYCL semaphore sharing modes. A single hung benchmark variant blocks the rest of the profiling sweep, so later configurations are never captured and the HTML analysis reports are never generated.

The benchmark tooling needs to detect unresponsive ChameleonRT runs, kill them, continue with the remaining variants, and clearly report missing data as failed driver/configuration combinations.

## Goals

- Prevent one hung benchmark variant from blocking the full sweep.
- Preserve successful benchmark data from other variants, backends, and resolutions.
- Record explicit failure metadata for timeout, process failure, and missing-artifact cases.
- Generate detailed and combined HTML reports even when some configurations fail.
- Mark failed configurations clearly in reports so broken driver behavior is visible.
- Keep the benchmark scripts portable and configurable.

## Proposed Solution

### 1. Add per-variant watchdog support to capture

Update `scripts/capture_benchmarks.py` so each benchmark variant is run independently with a timeout instead of using `subprocess.check_call(...)`.

Behavior:

- Launch ChameleonRT with `subprocess.Popen`.
- Wait for completion with a configurable timeout.
- If the process exceeds the timeout:
  - kill the full process tree,
  - mark the variant as failed due to timeout/hang,
  - continue to the next benchmark variant.
- If the process exits non-zero:
  - mark the variant as failed,
  - continue.
- If the process exits successfully but expected artifacts are missing:
  - mark the variant as failed due to missing output,
  - continue.

This prevents one broken SYCL semaphore path from blocking the whole sweep.

### 2. Make timeout configurable

Add an optional top-level capture config field:

```json
"benchmark_timeout_sec": 600
```

Allow per-variant overrides when needed:

```json
{
	"name": "timeline_semaphore",
	"benchmark_cmd": " -oidn-interop timeline_semaphore",
	"timeout_sec": 900
}
```

Update `benchmarks/run_oidn_benchmarks.ps1` to expose a parameter:

```powershell
-BenchmarkTimeoutSec 600
```

The PowerShell wrapper should inject that value into the temporary capture config generated for each backend/resolution.

### 3. Write explicit status metadata for each variant

For every variant directory, write:

```text
benchmark_status.json
```

Suggested fields:

- `name`
- `status`: `completed`, `timeout`, `process_failed`, or `missing_artifacts`
- `exit_code`
- `timeout_sec`
- `elapsed_sec`
- `crt_cmd_args`
- `message`
- `started_at`
- `ended_at`

Successful example:

```json
{
	"name": "baseline_host_blocking",
	"status": "completed",
	"message": "Benchmark completed successfully."
}
```

Timeout example:

```json
{
	"name": "timeline_semaphore",
	"status": "timeout",
	"message": "Benchmark exceeded 600 seconds and was killed."
}
```

This gives report scripts a reliable way to distinguish unknown missing data from a known failing driver/configuration.

### 4. Clean stale artifacts before each variant

Before launching each ChameleonRT run, delete stale files from the working directory:

- `benchmark_script.csv`
- `benchmark_script.json`

This avoids accidentally moving old results into a failed variant directory after a timeout or crash.

### 5. Capture process diagnostics

For each variant, capture process output into files in the variant directory, for example:

- `process.log`, or
- `stdout.log` and `stderr.log`

These diagnostics help identify driver/runtime failures when reporting regressions.

### 6. Make detailed HTML analysis tolerate failed/missing variants

Update `scripts/analyze_benchmarks.py` to:

- read `benchmark_status.json` when present,
- include failed variants in the benchmark description/status list,
- skip missing `benchmark.csv` in plots,
- render a visible failed-configurations section,
- avoid aborting when a configured variant has no data.

Plots should include only variants with valid CSV data, while the report still shows failed variants and their failure reasons.

### 7. Make relative plots robust

Relative plots currently assume the baseline data exists.

Update behavior so:

- if baseline data is missing, skip the relative plot and show a warning,
- if a compared variant is missing, omit that trace,
- report generation still succeeds.

This protects the report path if `baseline_host_blocking` fails or is interrupted.

### 8. Make combined summary report show failed configurations

Update `scripts/summarize_benchmarks.py` so `load_backend(...)` also loads variant directories containing `benchmark_status.json`, even if `benchmark.csv` is absent.

The summary should render failed rows, for example:

| Variant | Status | app_time_ms | vs baseline | Failure reason |
| --- | --- | ---: | ---: | --- |
| `baseline_host_blocking` | completed | 22.40 | — | — |
| `timeline_semaphore` | timeout | — | failed | Timeout after 600 seconds |
| `binary_semaphore` | completed | 18.70 | -16.5% | — |

Add a visible failed badge/style so broken driver configurations are easy to spot.

### 9. Keep the sweep moving at the wrapper level

Once `capture_benchmarks.py` treats per-variant failures as recoverable, `benchmarks/run_oidn_benchmarks.ps1` should not throw just because one variant failed.

Recommended behavior:

- Capture exits `0` if the script completed the sweep, even with failed variants.
- Capture exits non-zero only for unrecoverable setup/config errors.
- Analysis runs if at least one variant directory exists.
- Combined summary is generated even when some configs failed.

## Recommended Implementation Phases

### Phase 1: Capture watchdog and metadata

Implement the process watchdog in `scripts/capture_benchmarks.py`.

Deliverables:

- Configurable top-level timeout.
- Optional per-variant timeout override.
- Process launch through `subprocess.Popen`.
- Timeout detection and process killing.
- Stale artifact cleanup before each variant.
- `benchmark_status.json` for completed and failed variants.
- Process output logs per variant.

Validation:

- Run with a very small timeout to force a timeout.
- Confirm the process is killed.
- Confirm the next variant starts.
- Confirm failure metadata is written.
- Confirm successful variants still produce normal `benchmark.csv` and `benchmark.json`.

### Phase 2: PowerShell wrapper integration

Update `benchmarks/run_oidn_benchmarks.ps1`.

Deliverables:

- Add `-BenchmarkTimeoutSec` parameter.
- Inject timeout into generated temporary capture configs.
- Ensure capture failures caused by individual variants do not abort the entire backend/resolution sweep.
- Preserve existing `-SkipCapture`, `-SkipAnalysis`, and summary behavior.

Validation:

- Run one backend/resolution with forced timeout.
- Confirm remaining variants/resolutions continue.
- Confirm summary arguments are still accumulated for data directories containing partial results.

### Phase 3: Detailed report resilience

Update `scripts/analyze_benchmarks.py`.

Deliverables:

- Load `benchmark_status.json`.
- Skip missing CSVs without crashing.
- Render failed-configurations section.
- Make plot generation tolerate missing variants.
- Make relative plot generation tolerate missing baseline or compared variants.

Validation:

- Generate a report from partial data.
- Confirm completed variants are plotted.
- Confirm failed variants are listed with reasons.
- Confirm missing baseline produces a clear warning instead of a crash.

### Phase 4: Combined summary failure visibility

Update `scripts/summarize_benchmarks.py`.

Deliverables:

- Load status-only variant directories.
- Add status/failure columns or visible failure badges.
- Keep mean calculations for completed variants unchanged.
- Keep takeaways robust when some variants have no data.

Validation:

- Generate combined summary from mixed completed/failed data.
- Confirm failed rows are visible.
- Confirm fastest/takeaway logic ignores failed variants.
- Confirm report links still work for detailed reports.

### Phase 5: End-to-end benchmark sweep validation

Run an end-to-end controlled sweep.

Deliverables:

- One forced-timeout test run.
- One normal short sweep.
- Verified detailed reports.
- Verified combined summary.
- Verified diagnostics logs for failed variants.

Validation checklist:

1. Hung configuration is killed after timeout.
2. Remaining variants continue.
3. Remaining resolutions/backends continue.
4. Detailed HTML report is generated with partial data.
5. Combined HTML summary is generated with failed configurations marked.
6. Successful benchmark results remain unchanged in format and values.
