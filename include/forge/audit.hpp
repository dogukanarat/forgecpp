#pragma once
// Audit trail: who changed what, when, from where (web, link, console,
// telnet) and the old and new value. Every control change, button press,
// command call, alarm acknowledgement and lease goes through
// forge::ControlService, which records it here.
//
// Kept in memory ([audit].history, default 500), optionally appended to a
// JSON-lines file ([audit].file) and published on the "audit" event topic.
// The file is written by a thread of its own: record() never waits for a disk.

#include "forge/json.hpp"
#include "forge/subscription.hpp"
#include "forge/trace.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
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

    ~Audit();

    /// Wait until every record so far is in the file (tests, shutdown).
    void flush();

private:
    void write_loop(const std::stop_token& stop);

    mutable std::mutex mutex_;
    std::size_t max_;
    std::deque<Record> records_;
    Listeners<Record> listeners_;

    // The writer thread: lines queued by record(), written and flushed by write_loop().
    std::mutex file_mutex_;
    std::condition_variable_any file_ready_;
    std::deque<std::string> lines_;
    std::ofstream file_;
    std::string file_path_;
    bool open_pending_ = false;  // file_path_ changed: open it on the writer
    std::uint64_t queued_ = 0, written_ = 0;
    std::jthread writer_;
};

json to_json(const Audit::Record& r);

}  // namespace forge
