#pragma once
// Audit trail: who changed what, when, from where (web, link, console,
// telnet) and the old and new value. Every control change, button press,
// command call, alarm acknowledgement and lease goes through
// forge::ControlService, which records it here.
//
// Kept in memory ([audit].history, default 500), optionally appended to a
// JSON-lines file ([audit].file) and published on the "audit" event topic.

#include "forge/json.hpp"
#include "forge/subscription.hpp"
#include "forge/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace forge {

class Audit {
public:
    struct Record {
        std::int64_t time_unix_ns = 0;  ///< 0: set when recorded
        std::string channel;  ///< web | link | console | telnet | internal
        std::string client;   ///< address or client name
        std::string user;     ///< authenticated user, if any
        std::string action;   ///< set | press | call | ack | lease | release | log-level
        std::string target;   ///< control, command or alarm id
        json old_value;
        json new_value;
        bool ok = true;
        std::string error;
        trace::Context trace;
    };

    explicit Audit(std::size_t history = 500);

    void configure(std::size_t history, const std::string& file);
    void record(Record record);
    /// Newest last; limit 0 = all kept.
    std::vector<Record> list(std::size_t limit = 0) const;
    /// Called after each record, on the thread that recorded it.
    [[nodiscard]] Subscription on_record(std::function<void(const Record&)> listener) {
        return listeners_.add(std::move(listener));
    }

private:
    mutable std::mutex mutex_;
    std::size_t max_;
    std::deque<Record> records_;
    std::ofstream file_;
    std::string file_path_;
    Listeners<Record> listeners_;
};

json to_json(const Audit::Record& r);

}  // namespace forge
