#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

// Static scene statistics emitted for -scene-report. The schema mirrors what
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

// One-shot system/launch description written to "<base>.json" when a benchmark
// run finishes. Mirrors the "system" and "launch" objects that
// analyze_benchmarks.py reads from each benchmark.json.
struct BenchmarkEnvironment {
    std::string cpu_brand;
    std::string gpu_brand;
    std::string display_frontend;
    std::string rt_backend;
    std::vector<std::string> cmdline;
    // ChameleonRT renders at the window resolution, so render and display
    // resolutions are normally identical, but both are emitted for schema parity.
    int render_width = 0;
    int render_height = 0;
    int display_width = 0;
    int display_height = 0;
};

// Per-frame sample written as a single CSV row. The field names map onto the
// CSV columns the analysis tooling plots via report_config "perf_metric".
struct BenchmarkFrameStats {
    float render_time_ms = 0;
    float app_time_ms = 0;
    float denoise_time_ms = 0;
    float tonemap_time_ms = 0;
    float rays_per_second = 0;
    // Progressive accumulation count, reset whenever the camera moves.
    size_t frames_accumulated = 0;
};

// Streams progressive per-frame benchmark data to "<base>.csv" and writes the
// "<base>.json" summary on finish(). The output is consumed by the
// analyze_benchmarks.py tooling to produce benchmark reports.
class BenchmarkRecorder {
public:
    // Opens "<output_base>.csv" and writes the column header. On I/O failure the
    // recorder becomes inactive and record()/finish() are no-ops.
    explicit BenchmarkRecorder(const std::string &output_base);
    ~BenchmarkRecorder();

    BenchmarkRecorder(const BenchmarkRecorder &) = delete;
    BenchmarkRecorder &operator=(const BenchmarkRecorder &) = delete;

    // True when the CSV stream is open and rows are being written.
    bool active() const;

    // Append one CSV row for the most recently rendered frame.
    void record(const BenchmarkFrameStats &frame);

    // Write the "<base>.json" summary. Called once after the render loop.
    void finish(const BenchmarkEnvironment &env);

private:
    std::string output_base;
    // Monotonic frame counter across the whole run; emitted as the "frames_total"
    // column that the plots use as their x-axis.
    size_t frames_total = 0;
    std::unique_ptr<std::ofstream> csv;
};
