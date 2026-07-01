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
from pathlib import Path

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

    subprocess.check_call(str(crt_executable_path) + crt_cmd_line)

    # Collect the produced benchmark artifacts into the variant directory.
    shutil.move(
        crt_working_dir / Path("benchmark_script.csv"),
        benchmark_variant_dir / Path("benchmark.csv"),
    )
    shutil.move(
        crt_working_dir / Path("benchmark_script.json"),
        benchmark_variant_dir / Path("benchmark.json"),
    )

    # Create benchmark metadata for the report.
    benchmark_meta = {}
    benchmark_meta["desc"] = ""
    if "desc" in benchmark_variants[i]:
        benchmark_meta["desc"] = benchmark_variants[i]["desc"]
    benchmark_meta["crt_working_dir"] = str(crt_working_dir)
    benchmark_meta["crt_executable_path"] = str(crt_executable_path)
    benchmark_meta["crt_cmd_args"] = crt_cmd_line
    with open(benchmark_variant_dir / "benchmark_meta.json", "w") as outfile:
        json.dump(benchmark_meta, outfile, sort_keys=False, indent=4)

print("Benchmark capture done!")
