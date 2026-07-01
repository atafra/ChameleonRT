# ChameleonRT benchmark capture script.
#
# Runs chameleonrt.exe for each configured benchmark variant and collects the
# benchmark artifacts it produces. Notable behaviors:
#   * Uses "crt_working_dir" / "crt_executable_rel_path" config keys.
#   * Builds a ChameleonRT command line: <exe> <backend> <scene> [options].
#   * Writes PNG frames (no EXR -> JPEG conversion), so this script has no heavy
#     image dependencies.
#
# Usage:
#   python capture_benchmarks.py <BENCHMARK_CONFIG.JSON> <BENCHMARK_DIR_NAME>
#
# The ChameleonRT executable must be able to find its runtime dependencies. When
# the OIDN denoiser is enabled the Intel oneAPI runtime (libmmd.dll, etc.) must
# be on PATH, e.g. by prepending
#   "C:\\Program Files (x86)\\Intel\\oneAPI\\compiler\\latest\\bin".

import json
import subprocess
import shutil
import sys
import os
import shlex
import signal
import time
from pathlib import Path
from datetime import datetime, timezone

SCRIPT_DIR = Path(__file__).parent

if len(sys.argv) < 3:
    print(f"usage: {sys.argv[0]} <BENCHMARK_CONFIG.JSON> <BENCHMARK_DIR_NAME>")
    sys.exit(1)

fname = sys.argv[1]
benchmark_dir_name = sys.argv[2]

with open(fname) as json_file:
    benchmark_config = json.load(json_file)

benchmark_info = {}
benchmark_info["title"] = Path(fname).stem
if "title" in benchmark_config:
    benchmark_info["title"] = benchmark_config["title"]

benchmark_root_dir = Path(benchmark_dir_name).resolve()
benchmark_root_dir.mkdir(parents=True, exist_ok=True)

# save benchmark info
with open(benchmark_root_dir / "benchmark_info.json", "w") as outfile:
    json.dump(benchmark_info, outfile, sort_keys=False, indent=4)

# Set ChameleonRT working directory
crt_working_dir = Path(benchmark_config["crt_working_dir"]).resolve()
crt_executable_path = crt_working_dir / Path(benchmark_config["crt_executable_rel_path"])
os.chdir(crt_working_dir)

benchmark_variants = benchmark_config["benchmark_configs"]


def utc_now_iso():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, data):
    with open(path, "w") as outfile:
        json.dump(data, outfile, sort_keys=False, indent=4)


def kill_process_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except Exception:
            proc.kill()


def clean_stale_artifacts():
    for artifact in ("benchmark_script.csv", "benchmark_script.json"):
        artifact_path = crt_working_dir / Path(artifact)
        if artifact_path.exists():
            artifact_path.unlink()


def write_status(benchmark_variant_dir, status):
    write_json(benchmark_variant_dir / "benchmark_status.json", status)


def make_process_args(executable_path, cmd_line):
    if os.name == "nt":
        return str(executable_path) + cmd_line
    return [str(executable_path)] + shlex.split(cmd_line)


def make_popen_kwargs():
    if os.name == "nt":
        return {}
    return {"start_new_session": True}


for i in range(len(benchmark_variants)):
    benchmark_variant_name = benchmark_variants[i]["name"]
    print(f"Running benchmark variant {benchmark_variant_name} ({i}/{len(benchmark_variants)})...")
    benchmark_variant_dir = benchmark_root_dir / Path(benchmark_variant_name)
    benchmark_variant_dir.mkdir(parents=True, exist_ok=True)

    # Compose the ChameleonRT command line. The profiling output base is fixed to
    # "benchmark_script" so the produced benchmark_script.csv / .json can be moved
    # into the per-variant directory below, matching the analysis script's
    # expectations (benchmark.csv / benchmark.json).
    crt_cmd_line = (
        benchmark_config.get("shared_benchmark_cmd_prefix", "")
        + benchmark_variants[i].get("benchmark_cmd", "")
        + benchmark_config.get("shared_benchmark_cmd_postfix", "")
        + " -profiling benchmark_script -profiling-fps 1"
    )
    if "benchmark_frames" in benchmark_config:
        crt_cmd_line += " -benchmark-frames " + str(benchmark_config["benchmark_frames"])

    # The scene report is written once into the benchmark root so analyze can find
    # scene_info.json next to the variant directories.
    scene_info_file = benchmark_root_dir / Path("scene_info")
    crt_cmd_line += f' -scene-report "{str(scene_info_file)}"'

    # Create benchmark metadata before launch so failed/hung configurations are
    # still represented in the report output.
    benchmark_meta = {}
    benchmark_meta["desc"] = ""
    if "desc" in benchmark_variants[i]:
        benchmark_meta["desc"] = benchmark_variants[i]["desc"]
    benchmark_meta["crt_working_dir"] = str(crt_working_dir)
    benchmark_meta["crt_executable_path"] = str(crt_executable_path)
    benchmark_meta["crt_cmd_args"] = crt_cmd_line
    write_json(benchmark_variant_dir / "benchmark_meta.json", benchmark_meta)

    timeout_sec = benchmark_variants[i].get(
        "timeout_sec", benchmark_config.get("benchmark_timeout_sec", None)
    )
    if timeout_sec is not None:
        timeout_sec = float(timeout_sec)

    clean_stale_artifacts()

    started_at = utc_now_iso()
    start_time = time.monotonic()
    process_log_path = benchmark_variant_dir / "process.log"
    cmd = str(crt_executable_path) + crt_cmd_line
    process_args = make_process_args(crt_executable_path, crt_cmd_line)
    exit_code = None
    status_name = "completed"
    status_message = "Benchmark completed successfully."

    with open(process_log_path, "w", encoding="utf-8", errors="replace") as process_log:
        process_log.write(cmd + "\n\n")
        process_log.flush()
        proc = subprocess.Popen(
            process_args,
            stdout=process_log,
            stderr=subprocess.STDOUT,
            **make_popen_kwargs(),
        )
        try:
            exit_code = proc.wait(timeout=timeout_sec)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc)
            try:
                exit_code = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                exit_code = None
            status_name = "timeout"
            status_message = f"Benchmark exceeded {timeout_sec:g} seconds and was killed."

    elapsed_sec = time.monotonic() - start_time
    ended_at = utc_now_iso()

    if status_name == "completed" and exit_code != 0:
        status_name = "process_failed"
        status_message = f"Benchmark process exited with code {exit_code}."

    csv_artifact = crt_working_dir / Path("benchmark_script.csv")
    json_artifact = crt_working_dir / Path("benchmark_script.json")
    if status_name == "completed" and (not csv_artifact.exists() or not json_artifact.exists()):
        status_name = "missing_artifacts"
        status_message = "Benchmark completed but did not produce benchmark_script.csv and benchmark_script.json."

    status = {
        "name": benchmark_variant_name,
        "status": status_name,
        "exit_code": exit_code,
        "timeout_sec": timeout_sec,
        "elapsed_sec": elapsed_sec,
        "crt_cmd_args": crt_cmd_line,
        "message": status_message,
        "started_at": started_at,
        "ended_at": ended_at,
    }
    write_status(benchmark_variant_dir, status)

    if status_name != "completed":
        print(f"Benchmark variant {benchmark_variant_name} failed: {status_message}")
        clean_stale_artifacts()
        continue

    # Collect the produced benchmark artifacts into the variant directory.
    shutil.move(
        crt_working_dir / Path("benchmark_script.csv"),
        benchmark_variant_dir / Path("benchmark.csv"),
    )
    shutil.move(
        crt_working_dir / Path("benchmark_script.json"),
        benchmark_variant_dir / Path("benchmark.json"),
    )

print("Benchmark capture done!")
