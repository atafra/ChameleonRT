#include "profiling.h"
#include <cstdio>
#include <fstream>
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
