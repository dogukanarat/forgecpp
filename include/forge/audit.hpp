#pragma once
// Audit trail: who changed what, when, from where (web, ZeroMQ, console,
// telnet, HTTP RPC) and the old and new value. Every control change, button
// press, command call, alarm acknowledgement and lease goes through
// forge::ControlService, which records it here.
//
// Kept in memory ([audit].history, default 500), optionally appended to a
// JSON-lines file ([audit].file) and published on the "audit" event topic.

#include "forge/v1/observability.pb.h"

#include <cstddef>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace forge {

class Audit {
public:
    using Listener = std::function<void(const v1::AuditRecord&)>;

    explicit Audit(std::size_t history = 500);

    void configure(std::size_t history, const std::string& file);
    void record(v1::AuditRecord record);
    /// Newest last; limit 0 = all kept.
    std::vector<v1::AuditRecord> list(std::size_t limit = 0) const;
    void on_record(Listener listener);

private:
    mutable std::mutex mutex_;
    std::size_t max_;
    std::deque<v1::AuditRecord> records_;
    std::ofstream file_;
    std::string file_path_;
    std::vector<Listener> listeners_;
};

}  // namespace forge
