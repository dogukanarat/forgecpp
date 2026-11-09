#include "forge/audit.hpp"

#include "forge/log.hpp"

#include <algorithm>
#include <chrono>

namespace forge {

Audit::Audit(std::size_t history) : max_(std::max<std::size_t>(history, 1)) {}

Audit::~Audit() {
    if (writer_.joinable()) {
        writer_.request_stop();
        file_ready_.notify_all();
        writer_.join();
    }
}

void Audit::configure(std::size_t history, const std::string& file) {
    {
        std::lock_guard lock(mutex_);
        max_ = std::max<std::size_t>(history, 1);
        while (records_.size() > max_) records_.pop_front();
    }
    std::lock_guard lock(file_mutex_);
    if (file == file_path_) return;
    file_path_ = file;
    open_pending_ = true;
    if (!writer_.joinable()) writer_ = std::jthread([this](std::stop_token stop) { write_loop(stop); });
    file_ready_.notify_all();
}

void Audit::record(Record r) {
    if (r.time_unix_ns == 0) {
        r.time_unix_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
    }
    {
        std::lock_guard lock(mutex_);
        records_.push_back(r);
        while (records_.size() > max_) records_.pop_front();
    }
    {
        std::lock_guard lock(file_mutex_);
        if (!file_path_.empty()) {
            lines_.push_back(to_json(r).dump(-1, ' ', false, json::error_handler_t::replace));
            ++queued_;
            file_ready_.notify_all();
        }
    }
    listeners_.notify(r);
}

void Audit::flush() {
    std::unique_lock lock(file_mutex_);
    file_ready_.wait(lock, [&] { return lines_.empty() || !writer_.joinable(); });
}

void Audit::write_loop(const std::stop_token& stop) {
    std::unique_lock lock(file_mutex_);
    for (;;) {
        if (!file_ready_.wait(lock, stop, [&] { return open_pending_ || !lines_.empty(); })) {
            if (lines_.empty()) break;  // stopped: write what is left, then go
        }
        if (open_pending_) {
            open_pending_ = false;
            file_.close();
            if (!file_path_.empty()) {
                file_.open(file_path_, std::ios::app);
                if (!file_) log::error("audit: cannot open {}", file_path_);
            }
        }
        std::deque<std::string> batch;
        batch.swap(lines_);
        const auto path = file_path_;
        lock.unlock();
        if (file_.is_open()) {
            for (const auto& line : batch) file_ << line << '\n';
            file_.flush();
            if (!file_) {
                log::error("audit: cannot write {}; the file is closed", path);
                file_.close();
            }
        }
        lock.lock();
        written_ += batch.size();
        file_ready_.notify_all();  // flush() waits for an empty queue
        if (stop.stop_requested() && lines_.empty()) break;
    }
}

std::vector<Audit::Record> Audit::list(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    const std::size_t n = limit == 0 ? records_.size() : std::min(limit, records_.size());
    return {records_.end() - static_cast<std::ptrdiff_t>(n), records_.end()};
}

json to_json(const Audit::Record& r) {
    json j{{"time_unix_ns", r.time_unix_ns},
           {"channel", r.channel},
           {"client", r.client},
           {"user", r.user},
           {"action", r.action},
           {"target", r.target},
           {"ok", r.ok},
           {"error", r.error}};
    if (!r.old_value.is_null()) j["old_value"] = r.old_value;
    if (!r.new_value.is_null()) j["new_value"] = r.new_value;
    if (r.trace.valid()) j["trace_id"] = r.trace.trace_hex();
    return j;
}

}  // namespace forge
