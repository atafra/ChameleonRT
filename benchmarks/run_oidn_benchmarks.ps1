<#
.SYNOPSIS
	One-shot capture + analysis of ChameleonRT OIDN synchronization-mode benchmarks
	(Vulkan and DXR) across a resolution sweep using the Sponza scene.

.DESCRIPTION
	Reproduces the benchmark described in benchmarks/OIDN_SYNC_BENCHMARK_PLAN.md on a
	fresh system. The script:
	  * resolves the repository root relative to its own location (portable across checkouts),
	  * verifies prerequisites (executable, scene, Python + analysis deps),
	  * prepends the Intel oneAPI runtime to PATH (needed by the OIDN SYCL device),
	  * disables externally-injected Vulkan validation layers for representative timings,
	  * runs capture_benchmarks.py then analyze_benchmarks.py for each backend and resolution.

	All work paths are resolved relative to the repo root and the capture configs, so only
	-OneApiBin (and the 'crt_executable_rel_path' inside the capture configs) is
	environment-specific. DXR is Windows-only; the Vulkan portion is otherwise portable.

.PARAMETER Backends
	Which backends to run. Default: vulkan, dxr.

.PARAMETER OneApiBin
	Path to the Intel oneAPI compiler 'bin' directory containing the OIDN SYCL runtime DLLs.

.PARAMETER Resolutions
	Which resolutions to run. Default: 720p, 1080p, 1440p.

.PARAMETER BenchmarkTitle
	Short benchmark title used in the timestamped output folder name.

.PARAMETER RunId
	Optional explicit output folder name. Defaults to '<BenchmarkTitle>-yyyyMMdd_HHmmss'.

.PARAMETER InstallDeps
	Run 'pip install pandas plotly chart_studio matplotlib' if the analysis dependencies are missing.

.PARAMETER SummaryOutput
	Optional path (relative to the repo root) of the combined HTML results summary generated after
	the per-backend reports. Defaults inside the timestamped report folder.

.PARAMETER BenchmarkTimeoutSec
	Per-variant ChameleonRT watchdog timeout in seconds. Hung variants are killed and reported as
	failed while the rest of the sweep continues.

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

	[ValidateSet('720p', '1080p', '1440p')]
	[string[]] $Resolutions = @('720p', '1080p', '1440p'),

	[string] $BenchmarkTitle = 'oidn_resolution_sweep',
	[string] $RunId = '',
	[string] $SummaryOutput = '',
	[int] $BenchmarkTimeoutSec = 60,

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

	$resolutionTable = [ordered]@{
		'720p'  = @{ Width = 1280; Height = 720;  Label = '720p' }
		'1080p' = @{ Width = 1920; Height = 1080; Label = '1080p' }
		'1440p' = @{ Width = 2560; Height = 1440; Label = '1440p' }
	}

	$tempConfigDir = Join-Path ([System.IO.Path]::GetTempPath()) 'chameleonrt_oidn_benchmarks'
	New-Item -ItemType Directory -Force -Path $tempConfigDir | Out-Null

	if ([string]::IsNullOrWhiteSpace($RunId)) {
		$RunId = "$BenchmarkTitle-$(Get-Date -Format 'yyyyMMdd_HHmmss')"
	}
	$runDataRoot = Join-Path 'benchmarks/data' $RunId
	$runReportsRoot = Join-Path 'benchmarks/reports' $RunId
	if ([string]::IsNullOrWhiteSpace($SummaryOutput)) {
		$SummaryOutput = Join-Path $runReportsRoot 'oidn_summary.html'
	}
	Write-Host "Benchmark run id: $RunId" -ForegroundColor Cyan
	Write-Host "Data root: $runDataRoot" -ForegroundColor Cyan
	Write-Host "Reports root: $runReportsRoot" -ForegroundColor Cyan
	Write-Host "Per-variant timeout: $BenchmarkTimeoutSec seconds" -ForegroundColor Cyan

	# Accumulates '<Label> <data-dir>' pairs for the combined HTML summary generated
	# after all backends have been processed.
	$summaryBackendArgs = @()

	# --- Prerequisites -------------------------------------------------------
	if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
		throw "Python was not found on PATH."
	}

	if (-not $SkipAnalysis) {
		$analysisDeps = @('pandas', 'plotly', 'chart_studio', 'matplotlib')
		$analysisImportList = ($analysisDeps -join ', ')
		# Probe analysis deps without letting NativeCommandError terminate the script
		# when $ErrorActionPreference='Stop'. We handle the result via $LASTEXITCODE.
		$oldEap = $ErrorActionPreference
		try {
			$ErrorActionPreference = 'Continue'
			& python -c "import $analysisImportList" *> $null
		}
		finally {
			$ErrorActionPreference = $oldEap
		}
		if ($LASTEXITCODE -ne 0) {
			if ($InstallDeps) {
				Write-Host "Installing Python analysis dependencies..." -ForegroundColor Yellow
				& python -m pip install @analysisDeps
				if ($LASTEXITCODE -ne 0) { throw "Failed to install analysis dependencies." }
			}
			else {
				$depsText = ($analysisDeps -join '/')
				$depsCmd = ($analysisDeps -join ' ')
				throw "Missing Python analysis deps ($depsText). " +
					  "Re-run with -InstallDeps, or 'pip install $depsCmd'."
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

	# --- Run each backend/resolution -----------------------------------------
	foreach ($name in $Backends) {
		$job = $allJobs[$name]
		Write-Host "`n=== Backend: $name ===" -ForegroundColor Green
		$backendDataRoot = Join-Path $runDataRoot (Split-Path -Leaf $job.Data)
		$backendReportsRoot = Join-Path $runReportsRoot (Split-Path -Leaf $job.Reports)

		# The executable path is the one environment-specific value in the capture config;
		# read it from there (single source of truth) and verify it exists.
		$cfg = Get-Content $job.Capture -Raw | ConvertFrom-Json
		$exe = Join-Path $RepoRoot $cfg.crt_executable_rel_path
		if (-not (Test-Path $exe)) {
			throw "Executable from '$($job.Capture)' not found: $exe. " +
				  "Edit 'crt_executable_rel_path' in the config for your build tree."
		}

		foreach ($resName in $Resolutions) {
			$res = $resolutionTable[$resName]
			$resLabel = $res.Label
			$resData = Join-Path $backendDataRoot $resName
			$resReports = Join-Path $backendReportsRoot $resName
			$resTitle = "OIDN Synchronization Modes - $($job.Label) (Sponza, $resLabel)"
			$resDesc = "Comparison of OIDN interop/synchronization modes on the $($job.Label) backend at $($res.Width)x$($res.Height)."

			$captureCfg = Get-Content $job.Capture -Raw | ConvertFrom-Json
			$captureCfg.title = "OIDN Sync Modes - $($job.Label) (Sponza, $resLabel)"
			$captureCfg.shared_benchmark_cmd_prefix = " $name ./Assets/Sponza/sponza.obj -img $($res.Width) $($res.Height)"
			$captureCfg | Add-Member -NotePropertyName benchmark_timeout_sec -NotePropertyValue $BenchmarkTimeoutSec -Force
			$captureCfgPath = Join-Path $tempConfigDir "capture_oidn_${name}_${resName}.json"
			$captureCfg | ConvertTo-Json -Depth 20 | Set-Content -Encoding ASCII $captureCfgPath

			$reportCfg = Get-Content $job.Report -Raw | ConvertFrom-Json
			$reportCfg.title = $resTitle
			$reportCfg.description = $resDesc
			$reportCfgPath = Join-Path $tempConfigDir "report_oidn_${name}_${resName}.json"
			$reportCfg | ConvertTo-Json -Depth 20 | Set-Content -Encoding ASCII $reportCfgPath

			Write-Host "`n--- Resolution: $resLabel ($($res.Width)x$($res.Height)) ---" -ForegroundColor DarkCyan

			if (-not $SkipCapture) {
				# Disable externally-injected Vulkan validation layers for representative timings
				# (no effect on DXR). The app itself never enables them.
				if ($job.DisableVulkanValidation) {
					$env:VK_LOADER_LAYERS_DISABLE = 'VK_LAYER_KHRONOS_validation'
				}
				else {
					Remove-Item Env:VK_LOADER_LAYERS_DISABLE -ErrorAction SilentlyContinue
				}

				Write-Host "Capturing -> $resData" -ForegroundColor Cyan
				& python scripts\capture_benchmarks.py $captureCfgPath $resData
				if ($LASTEXITCODE -ne 0) { throw "Capture failed for backend '$name' at '$resName' (exit $LASTEXITCODE)." }
			}

			if (-not $SkipAnalysis) {
				if (-not (Test-Path $resData)) {
					throw "No existing data at '$resData' to analyze. Run without -SkipCapture first."
				}
				Write-Host "Analyzing -> $resReports" -ForegroundColor Cyan
				& python scripts\analyze_benchmarks.py $reportCfgPath $resData $resReports
				if ($LASTEXITCODE -ne 0) { throw "Analysis failed for backend '$name' at '$resName' (exit $LASTEXITCODE)." }

				$reportHtml = Join-Path $RepoRoot (Join-Path $resReports 'benchmark_report.html')
				Write-Host "Report: $reportHtml" -ForegroundColor Green
			}

			# Include this backend/resolution in the combined summary if it has capture data.
			if (Test-Path $resData) {
				$summaryBackendArgs += @('--backend', "$($job.Label) $resLabel", $resData)
			}
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
		& python scripts\summarize_benchmarks.py --output $SummaryOutput --ignore-frames $ignoreFrames --title 'OIDN Synchronization Modes - Resolution Sweep' @summaryBackendArgs
		if ($LASTEXITCODE -ne 0) { throw "Summary generation failed (exit $LASTEXITCODE)." }
		Write-Host "Summary: $(Join-Path $RepoRoot $SummaryOutput)" -ForegroundColor Green
	}

	Write-Host "`nAll requested benchmarks complete." -ForegroundColor Green
}
finally {
	Pop-Location
}
