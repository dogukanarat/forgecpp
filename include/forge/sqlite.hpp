#pragma once
// SQLite, thinly (forgecpp::sqlite, libforgecpp-sqlite): a database file, its
// statements and transactions, with errors as exceptions.
//
//   forge::sqlite::Database db("/var/lib/app/data.db");
//   db.exec("CREATE TABLE IF NOT EXISTS t(k TEXT PRIMARY KEY, v REAL)");
//   auto put = db.prepare("INSERT OR REPLACE INTO t VALUES(?, ?)");
//   put.bind(1, "gain").bind(2, 3.5).run();
//   auto get = db.prepare("SELECT v FROM t WHERE k = ?");
//   if (get.bind(1, "gain").step()) double v = get.real(0);
//
// One Database is one connection: use it from one thread at a time (a
// mutex around it, or one connection per thread). A query that takes long
// belongs off the main loop (background(), a worker()).
//
// forge::Backlog (forge/backlog.hpp) is a store-and-forward queue on top of it.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace forge::sqlite {

/// An SQLite error: what failed, SQLite's message and its result code.
class Error : public std::runtime_error {
public:
    Error(const std::string& what, int code) : std::runtime_error(what), code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

class Statement {
public:
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement();

    /// Parameters are numbered from 1.
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, int value) { return bind(index, static_cast<std::int64_t>(value)); }
    Statement& bind(int index, double value);
    Statement& bind(int index, std::string_view text);
    Statement& bind(int index, const char* text) { return bind(index, std::string_view(text)); }
    Statement& bind(int index, std::span<const std::uint8_t> blob);
    Statement& bind(int index, std::nullptr_t);

    /// The next row (true), or the end (false). A statement without rows
    /// (INSERT, UPDATE) returns false once it ran.
    bool step();
    /// step() until the end, then reset(): for a statement without rows.
    void run();
    /// Ready to run again; the bound values stay.
    void reset();

    /// Columns of the current row, numbered from 0.
    std::int64_t integer(int column) const;
    double real(int column) const;
    std::string text(int column) const;
    std::vector<std::uint8_t> blob(int column) const;
    bool null(int column) const;

private:
    friend class Database;
    Statement(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    void check(int rc, const char* what) const;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

struct Options {
    bool read_only = false;
    bool create = true;  ///< create the file when it is not there
    /// Write-ahead log: readers do not wait for the writer, and a crash or a
    /// power cut leaves the last committed transaction (the default).
    bool wal = true;
    /// NORMAL (with WAL: durable at checkpoints, fast) or FULL (every commit
    /// on the disk before it returns).
    bool full_sync = false;
    /// How long a statement waits for another connection's lock.
    int busy_timeout_ms = 5000;
};

class Database {
public:
    explicit Database(const std::string& path, Options options = {});
    ~Database();
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    /// One or more statements without results.
    void exec(std::string_view sql);
    Statement prepare(std::string_view sql);
    std::int64_t last_insert_id() const;
    /// Rows changed by the last INSERT, UPDATE or DELETE.
    std::int64_t changes() const;
    const std::string& path() const { return path_; }
    sqlite3* native() const { return db_; }

private:
    sqlite3* db_ = nullptr;
    std::string path_;
};

/// BEGIN in the constructor; ROLLBACK in the destructor unless commit() ran.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

/// The SQLite library this program runs with ("3.45.1").
std::string version();

}  // namespace forge::sqlite
