<#
.SYNOPSIS
	One-shot capture + analysis of ChameleonRT OIDN synchronization-mode benchmarks
	(Vulkan and DXR) at a fixed resolution using the Sponza scene.

.DESCRIPTION
	Reproduces the benchmark described in benchmarks/OIDN_SYNC_BENCHMARK_PLAN.md on a
	fresh system. The script:
	  * resolves the repository root relative to its own location (portable across checkouts),
	  * verifies prerequisites (executable, scene, Python + analysis deps),
	  * prepends the Intel oneAPI runtime to PATH (needed by the OIDN SYCL device),
	  * disables externally-injected Vulkan validation layers for representative timings,
	  * runs capture_benchmarks.py then analyze_benchmarks.py for each backend.

	All work paths are resolved relative to the repo root and the capture configs, so only
	-OneApiBin (and the 'crt_executable_rel_path' inside the capture configs) is
	environment-specific. DXR is Windows-only; the Vulkan portion is otherwise portable.

.PARAMETER Backends
	Which backends to run. Default: vulkan, dxr.

.PARAMETER OneApiBin
	Path to the Intel oneAPI compiler 'bin' directory containing the OIDN SYCL runtime DLLs.

.PARAMETER InstallDeps
	Run 'pip install pandas plotly chart_studio' if the analysis dependencies are missing.

.PARAMETER SummaryOutput
	Path (relative to the repo root) of the combined HTML results summary generated after
	the per-backend reports. The summary uses only the Python standard library.

.PARAMETER SkipCapture
	Skip capture and (re)generate reports from existing data.

.PARAMETER SkipAnalysis
	Capture benchmark data but do not generate the HTML reports.

.EXAMPLE
	.\benchmarks\run_oidn_benchmarks.ps1

.EXAMPLE
	.\benchmarks\run_oidn_benchmarks.ps1 -Backends vulkan -InstallDeps

.EXAMPLE
	.\benchmarks\run_oidn_benchmarks.ps1 -OneApiBin "C:\Program Files (x86)\Intel\oneAPI\compiler\latest\bin"
#>
[CmdletBinding()]
param(
	[ValidateSet('vulkan', 'dxr')]
	[string[]] $Backends = @('vulkan', 'dxr'),

	[string] $OneApiBin = 'C:\Program Files (x86)\Intel\oneAPI\compiler\latest\bin',

	[string] $SummaryOutput = 'benchmarks/reports/oidn_summary.html',

	[switch] $InstallDeps,
	[switch] $SkipCapture,
	[switch] $SkipAnalysis
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# The script lives in <repo>/benchmarks; the repo root is its parent. capture_benchmarks.py
# uses crt_working_dir="." so all commands must run from the repo root.
$RepoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $RepoRoot
try {
	Write-Host "Repo root: $RepoRoot" -ForegroundColor Cyan

	# Per-backend job table: capture config, report config, data dir, reports dir, and
	# whether to suppress externally-injected Vulkan validation layers.
	$allJobs = [ordered]@{
		vulkan = @{
			Label                   = 'Vulkan'
			Capture                 = 'benchmarks/configs/capture_oidn_vulkan.json'
			Report                  = 'benchmarks/configs/report_oidn_vulkan.json'
			Data                    = 'benchmarks/data/oidn_vulkan'
			Reports                 = 'benchmarks/reports/oidn_vulkan'
			DisableVulkanValidation = $true
		}
		dxr    = @{
			Label                   = 'DXR'
			Capture                 = 'benchmarks/configs/capture_oidn_dxr.json'
			Report                  = 'benchmarks/configs/report_oidn_dxr.json'
			Data                    = 'benchmarks/data/oidn_dxr'
			Reports                 = 'benchmarks/reports/oidn_dxr'
			DisableVulkanValidation = $false
		}
	}

	# Accumulates '<Label> <data-dir>' pairs for the combined HTML summary generated
	# after all backends have been processed.
	$summaryBackendArgs = @()

	# --- Prerequisites -------------------------------------------------------
	if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
		throw "Python was not found on PATH."
	}

	if (-not $SkipAnalysis) {
		& python -c "import pandas, plotly, chart_studio" *> $null
		if ($LASTEXITCODE -ne 0) {
			if ($InstallDeps) {
				Write-Host "Installing Python analysis dependencies..." -ForegroundColor Yellow
				& python -m pip install pandas plotly chart_studio
				if ($LASTEXITCODE -ne 0) { throw "Failed to install analysis dependencies." }
			}
			else {
				throw "Missing Python analysis deps (pandas/plotly/chart_studio). " +
					  "Re-run with -InstallDeps, or 'pip install pandas plotly chart_studio'."
			}
		}
	}

	if (-not (Test-Path '.\Assets\Sponza\sponza.obj')) {
		throw "Scene not found: Assets\Sponza\sponza.obj (run from a checkout that contains it)."
	}

	# The OIDN SYCL device needs the Intel oneAPI runtime DLLs on PATH.
	if (Test-Path $OneApiBin) {
		$env:PATH = "$OneApiBin;$env:PATH"
		Write-Host "oneAPI runtime prepended to PATH: $OneApiBin" -ForegroundColor Cyan
	}
	else {
		Write-Warning "oneAPI bin not found at '$OneApiBin'. If OIDN fails to initialize, pass -OneApiBin <path>."
	}

	# --- Run each backend ----------------------------------------------------
	foreach ($name in $Backends) {
		$job = $allJobs[$name]
		Write-Host "`n=== Backend: $name ===" -ForegroundColor Green

		# The executable path is the one environment-specific value in the capture config;
		# read it from there (single source of truth) and verify it exists.
		$cfg = Get-Content $job.Capture -Raw | ConvertFrom-Json
		$exe = Join-Path $RepoRoot $cfg.crt_executable_rel_path
		if (-not (Test-Path $exe)) {
			throw "Executable from '$($job.Capture)' not found: $exe. " +
				  "Edit 'crt_executable_rel_path' in the config for your build tree."
		}

		if (-not $SkipCapture) {
			# Disable externally-injected Vulkan validation layers for representative timings
			# (no effect on DXR). The app itself never enables them.
			if ($job.DisableVulkanValidation) {
				$env:VK_LOADER_LAYERS_DISABLE = 'VK_LAYER_KHRONOS_validation'
			}
			else {
				Remove-Item Env:VK_LOADER_LAYERS_DISABLE -ErrorAction SilentlyContinue
			}

			Write-Host "Capturing -> $($job.Data)" -ForegroundColor Cyan
			& python scripts\capture_benchmarks.py $job.Capture $job.Data
			if ($LASTEXITCODE -ne 0) { throw "Capture failed for backend '$name' (exit $LASTEXITCODE)." }
		}

		if (-not $SkipAnalysis) {
			if (-not (Test-Path $job.Data)) {
				throw "No existing data at '$($job.Data)' to analyze. Run without -SkipCapture first."
			}
			Write-Host "Analyzing -> $($job.Reports)" -ForegroundColor Cyan
			& python scripts\analyze_benchmarks.py $job.Report $job.Data $job.Reports
			if ($LASTEXITCODE -ne 0) { throw "Analysis failed for backend '$name' (exit $LASTEXITCODE)." }

			$reportHtml = Join-Path $RepoRoot (Join-Path $job.Reports 'benchmark_report.html')
			Write-Host "Report: $reportHtml" -ForegroundColor Green
		}

		# Include this backend in the combined summary if it has capture data.
		if (Test-Path $job.Data) {
			$summaryBackendArgs += @('--backend', $job.Label, $job.Data)
		}
	}

	# --- Combined HTML results summary --------------------------------------
	# Generated with the Python standard library only, so it works even when the
	# analysis dependencies are absent (e.g. -SkipAnalysis runs).
	if ($summaryBackendArgs.Count -gt 0) {
		# Reuse the per-backend report's ignore_frames so the summary means match the
		# detailed reports. All report configs use the same value; read the first one.
		$ignoreFrames = 0
		$firstReport = $allJobs[$Backends[0]].Report
		if (Test-Path $firstReport) {
			$rc = Get-Content $firstReport -Raw | ConvertFrom-Json
			if ($rc.PSObject.Properties.Name -contains 'ignore_frames') {
				$ignoreFrames = [int] $rc.ignore_frames
			}
		}

		Write-Host "`nGenerating combined summary -> $SummaryOutput" -ForegroundColor Cyan
		& python scripts\summarize_benchmarks.py --output $SummaryOutput --ignore-frames $ignoreFrames @summaryBackendArgs
		if ($LASTEXITCODE -ne 0) { throw "Summary generation failed (exit $LASTEXITCODE)." }
		Write-Host "Summary: $(Join-Path $RepoRoot $SummaryOutput)" -ForegroundColor Green
	}

	Write-Host "`nAll requested benchmarks complete." -ForegroundColor Green
}
finally {
	Pop-Location
}
