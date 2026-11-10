#include "forge/backlog.hpp"

#include "forge/sqlite.hpp"

#include <algorithm>
#include <stdexcept>

namespace forge {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool plain_name(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

}  // namespace

Backlog::Backlog(const std::string& path, BacklogOptions options) : path_(path), options_(std::move(options)) {
    if (!plain_name(options_.table)) throw std::invalid_argument("Backlog: not a table name: " + options_.table);
    if (options_.max_records == 0) throw std::invalid_argument("Backlog: max_records must be at least 1");
    db_ = std::make_unique<sqlite::Database>(path, sqlite::Options{.full_sync = options_.full_sync});
    const auto& t = options_.table;
    // AUTOINCREMENT: an id is never used twice, also after the newest record went.
    db_->exec("CREATE TABLE IF NOT EXISTS " + t +
              " (id INTEGER PRIMARY KEY AUTOINCREMENT, time_ms INTEGER NOT NULL, topic TEXT NOT NULL,"
              " data BLOB NOT NULL)");
    auto count = db_->prepare("SELECT COUNT(*), COALESCE(SUM(LENGTH(data)), 0) FROM " + t);
    if (count.step()) {
        size_ = static_cast<std::size_t>(count.integer(0));
        bytes_ = static_cast<std::size_t>(count.integer(1));
    }
}

Backlog::~Backlog() = default;

std::int64_t Backlog::append(std::string_view topic, std::span<const std::uint8_t> data) {
    std::lock_guard lock(mutex_);
    sqlite::Transaction tx(*db_);
    db_->prepare("INSERT INTO " + options_.table + " (time_ms, topic, data) VALUES (?, ?, ?)")
        .bind(1, now_ms())
        .bind(2, topic)
        .bind(3, data)
        .run();
    const auto id = db_->last_insert_id();
    ++size_;
    bytes_ += data.size();
    trim();
    tx.commit();
    return id;
}

std::int64_t Backlog::append(std::string_view topic, std::string_view text) {
    return append(topic, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void Backlog::trim() {
    if (size_ <= options_.max_records && bytes_ <= options_.max_bytes) return;
    // The oldest first, a few at a time, until within both limits (the newest record always stays).
    auto oldest = db_->prepare("SELECT id, LENGTH(data) FROM " + options_.table + " ORDER BY id LIMIT 64");
    auto remove = db_->prepare("DELETE FROM " + options_.table + " WHERE id <= ?");
    while ((size_ > options_.max_records || bytes_ > options_.max_bytes) && size_ > 1) {
        std::int64_t last = -1;
        while ((size_ > options_.max_records || bytes_ > options_.max_bytes) && size_ > 1 && oldest.step()) {
            last = oldest.integer(0);
            bytes_ -= std::min(bytes_, static_cast<std::size_t>(oldest.integer(1)));
            --size_;
            ++dropped_;
        }
        oldest.reset();
        if (last < 0) break;
        remove.bind(1, last).run();
    }
}

std::vector<Backlog::Record> Backlog::peek(std::size_t max_records, std::size_t max_bytes) const {
    std::lock_guard lock(mutex_);
    std::vector<Record> out;
    if (max_records == 0) return out;
    auto q = db_->prepare("SELECT id, time_ms, topic, data FROM " + options_.table + " ORDER BY id LIMIT ?");
    q.bind(1, static_cast<std::int64_t>(std::min<std::size_t>(max_records, 1u << 30)));
    std::size_t bytes = 0;
    while (q.step()) {
        Record r{q.integer(0), q.integer(1), q.text(2), q.blob(3)};
        if (!out.empty() && bytes + r.data.size() > max_bytes) break;
        bytes += r.data.size();
        out.push_back(std::move(r));
    }
    return out;
}

void Backlog::remove_through(std::int64_t id) {
    std::lock_guard lock(mutex_);
    sqlite::Transaction tx(*db_);
    auto gone =
        db_->prepare("SELECT COUNT(*), COALESCE(SUM(LENGTH(data)), 0) FROM " + options_.table + " WHERE id <= ?");
    gone.bind(1, id);
    std::size_t n = 0, b = 0;
    if (gone.step()) {
        n = static_cast<std::size_t>(gone.integer(0));
        b = static_cast<std::size_t>(gone.integer(1));
    }
    db_->prepare("DELETE FROM " + options_.table + " WHERE id <= ?").bind(1, id).run();
    tx.commit();
    size_ -= std::min(size_, n);
    bytes_ -= std::min(bytes_, b);
}

std::size_t Backlog::size() const {
    std::lock_guard lock(mutex_);
    return size_;
}

std::size_t Backlog::bytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

std::uint64_t Backlog::dropped() const {
    std::lock_guard lock(mutex_);
    return dropped_;
}

}  // namespace forge
