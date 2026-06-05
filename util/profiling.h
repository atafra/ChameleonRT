#pragma once

#include <cstddef>
#include <string>

// Static scene statistics emitted for --scene-report. The schema mirrors what
// the benchmark analysis tooling (analyze_benchmarks.py) consumes: the values
// are nested under a top-level "info" object.
struct SceneReport {
    size_t unique_tris = 0;
    size_t total_tris = 0;
    size_t num_param_meshes = 0;
    size_t num_instances = 0;
    // ChameleonRT has no level-of-detail concept, so this is always 0. It is
    // emitted to keep the JSON schema compatible with the analysis scripts.
    size_t num_lod_groups = 0;
};

// Writes "<path_no_ext>.json" containing the scene statistics. The extension is
// appended here so callers pass the same base path the scripts expect (e.g.
// "scene_info"). Returns false on I/O failure.
bool write_scene_report(const std::string &path_no_ext, const SceneReport &report);
