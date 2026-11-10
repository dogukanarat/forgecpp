#pragma once
// forge::Backlog (forgecpp::sqlite): store and forward. Records wait in an
// SQLite file until the other end has them: telemetry kept while there is no
// ground contact, sent in order during the next pass, still there after a
// restart or a power cut.
//
//   forge::Backlog backlog("/var/lib/app/backlog.db", {.max_records = 100000});
//   backlog.append("hk", bytes);                    // any thread
//   ...
//   auto batch = backlog.peek(100, 64 * 1024);      // the oldest first
//   if (send(batch)) backlog.remove_through(batch.back().id);   // after the other end has them
//
// When it is full (max_records or max_bytes), the oldest records go first
// and dropped() counts them: the newest data is the most useful one.
// Thread safe; each call is one SQLite transaction (a few hundred
// microseconds on flash): call it from a worker() or background() when the
// rate is high.

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

namespace sqlite {
class Database;
}  // namespace sqlite

struct BacklogOptions {
    std::size_t max_records = 1'000'000;
    std::size_t max_bytes = 256u * 1024 * 1024;  ///< of the records' data
    std::string table = "backlog";
    bool full_sync = false;  ///< every append on the disk before it returns (slower)
};

class Backlog {
public:
    struct Record {
        std::int64_t id = 0;       ///< increasing; remove_through(id) acknowledges up to it
        std::int64_t time_ms = 0;  ///< wall clock (Unix ms) when it was appended
        std::string topic;
        std::vector<std::uint8_t> data;
    };

    explicit Backlog(const std::string& path, BacklogOptions options = {});
    ~Backlog();
    Backlog(const Backlog&) = delete;
    Backlog& operator=(const Backlog&) = delete;

    /// Keep a record; returns its id.
    std::int64_t append(std::string_view topic, std::span<const std::uint8_t> data);
    std::int64_t append(std::string_view topic, std::string_view text);

    /// The oldest records, at most `max_records` and about `max_bytes` of data
    /// (at least one record when there is one, however large).
    std::vector<Record> peek(std::size_t max_records,
                             std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) const;
    /// The other end has every record up to `id`: they go.
    void remove_through(std::int64_t id);

    std::size_t size() const;
    std::size_t bytes() const;
    /// Records dropped to keep within the limits since the backlog was opened.
    std::uint64_t dropped() const;
    const std::string& path() const { return path_; }

private:
    void trim();  // with mutex_ held

    std::string path_;
    BacklogOptions options_;
    mutable std::mutex mutex_;
    std::unique_ptr<sqlite::Database> db_;
    std::size_t size_ = 0;
    std::size_t bytes_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace forge
