#include "forge/audit.hpp"

#include "forge/proto.hpp"

#include <algorithm>

namespace forge {

Audit::Audit(std::size_t history) : max_(std::max<std::size_t>(history, 1)) {}

void Audit::configure(std::size_t history, const std::string& file) {
    std::lock_guard lock(mutex_);
    max_ = std::max<std::size_t>(history, 1);
    while (records_.size() > max_) records_.pop_front();
    if (file != file_path_) {
        file_.close();
        file_path_ = file;
        if (!file.empty()) file_.open(file, std::ios::app);
    }
}

void Audit::record(v1::AuditRecord r) {
    if (r.time_unix_ns() == 0) r.set_time_unix_ns(proto::now_unix_ns());
    std::vector<Listener> listeners;
    {
        std::lock_guard lock(mutex_);
        records_.push_back(r);
        while (records_.size() > max_) records_.pop_front();
        if (file_.is_open()) {
            try {
                file_ << proto::to_json_string(r) << '\n';
                file_.flush();
            } catch (...) {
            }
        }
        listeners = listeners_;
    }
    for (auto& l : listeners) {
        try {
            l(r);
        } catch (...) {
        }
    }
}

std::vector<v1::AuditRecord> Audit::list(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    const std::size_t n = limit == 0 ? records_.size() : std::min(limit, records_.size());
    return {records_.end() - static_cast<std::ptrdiff_t>(n), records_.end()};
}

void Audit::on_record(Listener listener) {
    std::lock_guard lock(mutex_);
    listeners_.push_back(std::move(listener));
}

}  // namespace forge
