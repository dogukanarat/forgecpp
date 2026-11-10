#pragma once
// forge/orm.hpp: SQLite tables as C++ structs, with sqlite_orm (forgecpp::orm).
//
// sqlite_orm maps a struct to a table and writes the SQL for you: the
// queries are C++ expressions, checked by the compiler. forge::orm::open()
// gives a storage the settings forge::sqlite::Database uses (one connection
// kept open, WAL, a busy timeout, synchronous=NORMAL) and creates or updates
// the tables, refusing an update that would lose rows (Options::allow_data_loss).
//
//   struct Fault { std::int64_t id = 0; std::string text; std::optional<std::int64_t> cleared_ms; };
//
//   inline auto make_db(const std::string& path) {
//       using namespace sqlite_orm;
//       return make_storage(path, make_table("faults",
//           make_column("id", &Fault::id, primary_key().autoincrement()),
//           make_column("text", &Fault::text),
//           make_column("cleared_ms", &Fault::cleared_ms)));
//   }
//   using Db = decltype(make_db(""));
//
//   Db db = make_db("faults.db");
//   forge::orm::open(db);
//   const auto id = db.insert(Fault{.text = "battery low"});
//   auto open_faults = db.get_all<Fault>(where(is_null(&Fault::cleared_ms)), order_by(&Fault::id).desc());
//
// A storage is not thread safe: use it from one thread (the main loop, or
// one worker). Errors are std::system_error exceptions.
//
// License: sqlite_orm is AGPL-3.0, or MIT when bought from its author
// (https://github.com/fnc12/sqlite_orm). The header is compiled into the
// program that includes it, so that program takes on the license; forgecpp
// itself and programs that do not include this header do not.
// See docs/storage.md.

#include <sqlite_orm/sqlite_orm.h>

#include <map>
#include <stdexcept>
#include <string>

namespace forge::orm {

struct Options {
    /// Write-ahead log: readers do not wait for the writer, a second
    /// connection (or another program) may read meanwhile.
    bool wal = true;
    /// synchronous=FULL: every commit on the disk before it returns (slower).
    /// NORMAL keeps the database sound after a power cut, and may lose the
    /// last commits.
    bool full_sync = false;
    /// How long a write waits for another connection's write.
    int busy_timeout_ms = 5000;
    /// Create missing tables and columns, drop columns the structs no longer
    /// have; see sync_schema() in sqlite_orm.
    bool sync_schema = true;
    /// sqlite_orm drops a table and makes it again, rows and all, when it
    /// cannot change it in place: mostly a new column that is NOT NULL and
    /// has no default (a plain member without default_value()). open() checks
    /// first and refuses (std::runtime_error) unless this is true. Give a new
    /// column a default_value(), or make it std::optional, and the rows stay.
    bool allow_data_loss = false;
};

/// Opens `storage` for good and applies `options`; returns what
/// sync_schema() did to each table (empty without it).
template <class Storage>
std::map<std::string, sqlite_orm::sync_schema_result> open(Storage& storage, const Options& options = {}) {
    // Otherwise sqlite_orm opens and closes the file around every query: slow,
    // and the settings below would be lost each time.
    storage.open_forever();
    storage.busy_timeout(options.busy_timeout_ms);
    if (options.wal) storage.pragma.journal_mode(sqlite_orm::journal_mode::WAL);
    storage.pragma.synchronous(options.full_sync ? 2 : 1);
    if (!options.sync_schema) return {};
    if (!options.allow_data_loss) {
        for (const auto& [table, change] : storage.sync_schema_simulate(true)) {
            if (change == sqlite_orm::sync_schema_result::dropped_and_recreated) {
                throw std::runtime_error("forge::orm: table \"" + table +
                                         "\" would be dropped and made again, its rows lost (a new column "
                                         "that is NOT NULL without a default?). Give the column a "
                                         "default_value() or make it std::optional; or set allow_data_loss");
            }
        }
    }
    return storage.sync_schema(true);
}

/// The sqlite_orm version forgecpp was built and tested with.
inline constexpr const char* tested_version = "1.9.1";

}  // namespace forge::orm
