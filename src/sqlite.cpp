#include "forge/sqlite.hpp"

#include <sqlite3.h>

#include <utility>

namespace forge::sqlite {
namespace {

[[noreturn]] void raise(sqlite3* db, int rc, const std::string& what) {
    const char* message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    throw Error("sqlite: " + what + ": " + (message ? message : "error"), rc);
}

}  // namespace

// --- Statement -------------------------------------------------------------------------

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(stmt_);
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::check(int rc, const char* what) const {
    if (rc != SQLITE_OK) raise(db_, rc, what);
}

Statement& Statement::bind(int index, std::int64_t value) {
    check(sqlite3_bind_int64(stmt_, index, value), "bind");
    return *this;
}

Statement& Statement::bind(int index, double value) {
    check(sqlite3_bind_double(stmt_, index, value), "bind");
    return *this;
}

Statement& Statement::bind(int index, std::string_view text) {
    check(sqlite3_bind_text64(stmt_, index, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8), "bind");
    return *this;
}

Statement& Statement::bind(int index, std::span<const std::uint8_t> blob) {
    // A zero-length blob is a blob, not NULL: SQLite needs a non-null pointer for that.
    static const std::uint8_t empty = 0;
    check(sqlite3_bind_blob64(stmt_, index, blob.empty() ? &empty : blob.data(), blob.size(), SQLITE_TRANSIENT),
          "bind");
    return *this;
}

Statement& Statement::bind(int index, std::nullptr_t) {
    check(sqlite3_bind_null(stmt_, index), "bind");
    return *this;
}

bool Statement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    raise(db_, rc, "step");
}

void Statement::run() {
    while (step()) {
    }
    reset();
}

void Statement::reset() { sqlite3_reset(stmt_); }

std::int64_t Statement::integer(int column) const { return sqlite3_column_int64(stmt_, column); }

double Statement::real(int column) const { return sqlite3_column_double(stmt_, column); }

std::string Statement::text(int column) const {
    const auto* p = sqlite3_column_text(stmt_, column);
    const int n = sqlite3_column_bytes(stmt_, column);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n)) : std::string();
}

std::vector<std::uint8_t> Statement::blob(int column) const {
    const auto* p = static_cast<const std::uint8_t*>(sqlite3_column_blob(stmt_, column));
    const int n = sqlite3_column_bytes(stmt_, column);
    return p ? std::vector<std::uint8_t>(p, p + n) : std::vector<std::uint8_t>();
}

bool Statement::null(int column) const { return sqlite3_column_type(stmt_, column) == SQLITE_NULL; }

// --- Database --------------------------------------------------------------------------

Database::Database(const std::string& path, Options options) : path_(path) {
    int flags = SQLITE_OPEN_NOMUTEX;  // one thread at a time, the caller's promise
    flags |= options.read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (options.create && !options.read_only) flags |= SQLITE_OPEN_CREATE;
    const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error("sqlite: cannot open " + path + ": " + message, rc);
    }
    sqlite3_extended_result_codes(db_, 1);
    sqlite3_busy_timeout(db_, options.busy_timeout_ms);
    try {
        if (options.wal && !options.read_only) exec("PRAGMA journal_mode=WAL");
        exec(options.full_sync ? "PRAGMA synchronous=FULL" : "PRAGMA synchronous=NORMAL");
        exec("PRAGMA foreign_keys=ON");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Database::~Database() { sqlite3_close(db_); }

Database::Database(Database&& other) noexcept : db_(std::exchange(other.db_, nullptr)), path_(std::move(other.path_)) {}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        sqlite3_close(db_);
        db_ = std::exchange(other.db_, nullptr);
        path_ = std::move(other.path_);
    }
    return *this;
}

void Database::exec(std::string_view sql) {
    const std::string text(sql);
    char* message = nullptr;
    const int rc = sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string why = message ? message : sqlite3_errstr(rc);
        sqlite3_free(message);
        throw Error("sqlite: " + why + " (in: " + text.substr(0, 80) + ")", rc);
    }
}

Statement Database::prepare(std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK) raise(db_, rc, "cannot prepare \"" + std::string(sql.substr(0, 80)) + "\"");
    return Statement(db_, stmt);
}

std::int64_t Database::last_insert_id() const { return sqlite3_last_insert_rowid(db_); }

std::int64_t Database::changes() const { return sqlite3_changes64(db_); }

// --- Transaction -----------------------------------------------------------------------

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
    if (!done_) {
        try {
            db_.exec("ROLLBACK");
        } catch (...) {  // nothing to roll back (the error already ended it)
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

std::string version() { return sqlite3_libversion(); }

}  // namespace forge::sqlite
