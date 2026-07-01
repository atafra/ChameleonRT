#include "profiling.h"
#include <cstdio>
#include <fstream>
#include <ostream>
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

namespace {
// Dump a serialized JSON buffer to disk. Returns false on failure.
bool write_text(const std::string &path, const char *data, size_t len)
{
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "profiling: failed to open '%s' for writing\n", path.c_str());
        return false;
    }
    out.write(data, static_cast<std::streamsize>(len));
    return out.good();
}
}  // namespace

bool write_scene_report(const std::string &path_no_ext, const SceneReport &report)
{
    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();
    writer.Key("info");
    writer.StartObject();
    writer.Key("unique_tris");
    writer.Uint64(report.unique_tris);
    writer.Key("total_tris");
    writer.Uint64(report.total_tris);
    writer.Key("num_param_meshes");
    writer.Uint64(report.num_param_meshes);
    writer.Key("num_instances");
    writer.Uint64(report.num_instances);
    writer.Key("num_lod_groups");
    writer.Uint64(report.num_lod_groups);
    writer.EndObject();
    writer.EndObject();

    return write_text(path_no_ext + ".json", buffer.GetString(), buffer.GetSize());
}

BenchmarkRecorder::BenchmarkRecorder(const std::string &output_base) : output_base(output_base)
{
    csv.reset(new std::ofstream(output_base + ".csv", std::ios::out | std::ios::trunc));
    if (!csv || !*csv) {
        std::fprintf(stderr,
                     "profiling: failed to open '%s.csv' for writing; benchmark "
                     "recording disabled\n",
                     output_base.c_str());
        csv.reset();
        return;
    }

    // Standard columns followed by ChameleonRT's extended per-pass metrics.
    // The names must match the report_config "perf_metric" values the analysis
    // tooling plots, with "frames_total" used as the x-axis.
    *csv << "frames_total"
         << ",frames_accumulated"
         << ",render_time_ms"
         << ",app_time_ms"
         << ",denoise_time_ms"
         << ",tonemap_time_ms"
         << ",rays_per_second" << '\n';
}

BenchmarkRecorder::~BenchmarkRecorder() = default;

bool BenchmarkRecorder::active() const
{
    return csv && static_cast<bool>(*csv);
}

void BenchmarkRecorder::record(const BenchmarkFrameStats &frame)
{
    if (!active()) {
        return;
    }

    *csv << frames_total << ',' << frame.frames_accumulated << ',' << frame.render_time_ms
         << ',' << frame.app_time_ms << ',' << frame.denoise_time_ms << ','
         << frame.tonemap_time_ms << ',' << frame.rays_per_second << '\n';
    ++frames_total;
}

void BenchmarkRecorder::finish(const BenchmarkEnvironment &env)
{
    if (csv) {
        csv->flush();
        csv.reset();
    }

    rapidjson::StringBuffer buffer;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

    writer.StartObject();
    writer.Key("system");
    writer.StartObject();
    writer.Key("cpu");
    writer.String(env.cpu_brand.c_str());
    writer.Key("gpu");
    writer.String(env.gpu_brand.c_str());
    writer.Key("gpu_name");
    writer.String(env.gpu_name.c_str());
    writer.Key("gpu_driver_version");
    writer.String(env.gpu_driver_version.c_str());
    writer.Key("display");
    writer.String(env.display_frontend.c_str());
    writer.EndObject();

    writer.Key("app");
    writer.StartObject();
    writer.Key("backend");
    writer.String(env.rt_backend.c_str());
    writer.EndObject();

    writer.Key("launch");
    writer.StartObject();
    writer.Key("display_res");
    writer.StartArray();
    writer.Int(env.display_width);
    writer.Int(env.display_height);
    writer.EndArray();
    writer.Key("render_res");
    writer.StartArray();
    writer.Int(env.render_width);
    writer.Int(env.render_height);
    writer.EndArray();
    writer.Key("cmdline");
    writer.StartArray();
    for (const std::string &arg : env.cmdline) {
        writer.String(arg.c_str());
    }
    writer.EndArray();
    writer.EndObject();
    writer.EndObject();

    write_text(output_base + ".json", buffer.GetString(), buffer.GetSize());
}
