#include "forge/audit.hpp"

#include "forge/log.hpp"

#include <algorithm>
#include <chrono>

namespace forge {

Audit::Audit(std::size_t history) : max_(std::max<std::size_t>(history, 1)) {}

void Audit::configure(std::size_t history, const std::string& file) {
    std::lock_guard lock(mutex_);
    max_ = std::max<std::size_t>(history, 1);
    while (records_.size() > max_) records_.pop_front();
    if (file != file_path_) {
        file_.close();
        file_path_ = file;
        if (!file.empty()) {
            file_.open(file, std::ios::app);
            if (!file_) log::error("audit: cannot open {}", file);
        }
    }
}

void Audit::record(Record r) {
    if (r.time_unix_ns == 0) {
        r.time_unix_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    }
    {
        std::lock_guard lock(mutex_);
        records_.push_back(r);
        while (records_.size() > max_) records_.pop_front();
        if (file_.is_open()) {
            file_ << to_json(r).dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
            file_.flush();
            if (!file_) {
                log::error("audit: cannot write {}; the file is closed", file_path_);
                file_.close();
            }
        }
    }
    listeners_.notify(r);
}

std::vector<Audit::Record> Audit::list(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    const std::size_t n = limit == 0 ? records_.size() : std::min(limit, records_.size());
    return {records_.end() - static_cast<std::ptrdiff_t>(n), records_.end()};
}

json to_json(const Audit::Record& r) {
    json j{{"time_unix_ns", r.time_unix_ns}, {"channel", r.channel}, {"client", r.client}, {"user", r.user},
           {"action", r.action},             {"target", r.target},   {"ok", r.ok},         {"error", r.error}};
    if (!r.old_value.is_null()) j["old_value"] = r.old_value;
    if (!r.new_value.is_null()) j["new_value"] = r.new_value;
    if (r.trace.valid()) j["trace_id"] = r.trace.trace_hex();
    return j;
}

}  // namespace forge
