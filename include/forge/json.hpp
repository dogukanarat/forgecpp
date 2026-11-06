#pragma once
// JSON helpers built on nlohmann/json.

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string_view>

namespace forge {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

/// Read and parse a JSON file. Throws std::runtime_error / json::parse_error.
json load_json(const std::filesystem::path& path);

/// Write JSON atomically (temp file + fsync + rename), creating parent dirs.
void save_json(const std::filesystem::path& path, const json& value, int indent = 2);

/// Write a file atomically (temp file + fsync + rename), creating parent dirs.
void write_file_atomic(const std::filesystem::path& path, std::string_view content);

}  // namespace forge
