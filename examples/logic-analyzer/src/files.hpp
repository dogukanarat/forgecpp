#pragma once
// Captures on disk and the export formats.
//
//   .lacap   this program's own file: a header line of JSON, then the words
//            of the FPGA's format (wire.hpp). Small (4 bytes a change), and
//            opened again later (the Captures list, or an upload).
//   .vcd     Value Change Dump: GTKWave, PulseView/sigrok, most simulators.
//   .csv     one row per change: time and the four levels.

#include "capture.hpp"

#include <forge/json.hpp>

#include <filesystem>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

namespace la {

/// Writes `c` as a .lacap file (extra: anything to keep with it, e.g. the analyzers).
void write_capture(const Capture& c, std::ostream& out, const forge::json& extra = forge::json::object());
/// Reads a .lacap file; throws std::runtime_error when it is not one. `extra` gets what was kept.
Capture read_capture(std::istream& in, forge::json* extra = nullptr);

void write_vcd(const Capture& c, std::ostream& out);
void write_csv(const Capture& c, std::ostream& out);

/// The header of a capture for lists: id, time, length, changes, trigger, ...
forge::json capture_summary(const Capture& c);

/// A directory of .lacap files, oldest removed beyond `keep`.
class CaptureStore {
public:
    CaptureStore(std::filesystem::path dir, std::size_t keep);

    /// Saves it; returns the file name.
    std::string save(const Capture& c, const forge::json& extra = forge::json::object());
    /// [{file, bytes, ...summary}], newest first.
    forge::json list() const;
    /// The path of a file in the store; throws std::invalid_argument for a name outside it.
    std::filesystem::path path(const std::string& file) const;
    Capture load(const std::string& file, forge::json* extra = nullptr) const;
    bool remove(const std::string& file);
    const std::filesystem::path& dir() const { return dir_; }

private:
    void prune();

    std::filesystem::path dir_;
    std::size_t keep_;
    mutable std::mutex mutex_;
};

}  // namespace la
