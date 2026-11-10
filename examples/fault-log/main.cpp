// fault-log: a fault log kept in SQLite with sqlite_orm: tables as C++ structs,
// queries as C++ expressions, no SQL written by hand.
//
//   units    the parts of the device (EPS, ADCS, radio, payload), from [faults].units
//   faults   every fault a unit raised: when, its code, severity and text,
//            and when it cleared (NULL while it is open)
//
// A dummy fault source raises faults now and then ([faults].rate per check,
// every [faults].period_ms). Info and warnings clear themselves after
// [faults].clear_after_s; errors stay open until an operator clears them
// (the "Clear all errors" button, or `clear <id>` / `clear all` on the console).
// The log keeps the newest [faults].keep faults.
//
// Console (forge console, telnet, or the panel's console page):
//   faults [n]          the newest open faults
//   history [unit] [n]  the newest faults, open or not (of one unit)
//   stats               faults per unit, and open ones per severity
//   clear <id> | all    clear a fault, or every open one
//
// The file is plain SQLite; any SQLite tool reads it:
//   sqlite3 faults.db "SELECT * FROM faults ORDER BY id DESC LIMIT 5"
//
// sqlite_orm is AGPL-3.0 (or MIT when bought): see docs/storage.md.
//
//   ./fault-log -c fault-log.toml --http 8080

#include <forge/forge.hpp>
#include <forge/orm.hpp>

#include <cctype>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace faults {

enum Severity : int { info = 0, warning = 1, error = 2 };

inline const char* severity_name(int s) {
    switch (s) {
        case info: return "info";
        case warning: return "warning";
        default: return "error";
    }
}

// --- The tables: one struct each ------------------------------------------------------

struct Unit {
    std::int64_t id = 0;
    std::string name;
};

struct Fault {
    std::int64_t id = 0;
    std::int64_t unit_id = 0;
    std::int64_t time_ms = 0;  // UTC, milliseconds since 1970
    int code = 0;
    int severity = info;
    std::string text;
    std::optional<std::int64_t> cleared_ms;  // NULL: still open
};

// The schema, once: sqlite_orm makes the tables (and later columns) from it.
// clang-format off
inline auto make_db(const std::string& path) {
    using namespace sqlite_orm;
    return make_storage(
        path,
        make_index("faults_open", &Fault::cleared_ms),
        make_table("units",
                   make_column("id", &Unit::id, primary_key().autoincrement()),
                   make_column("name", &Unit::name, unique())),
        make_table("faults",
                   make_column("id", &Fault::id, primary_key().autoincrement()),
                   make_column("unit_id", &Fault::unit_id),
                   make_column("time_ms", &Fault::time_ms),
                   make_column("code", &Fault::code),
                   make_column("severity", &Fault::severity),
                   make_column("text", &Fault::text),
                   make_column("cleared_ms", &Fault::cleared_ms),
                   foreign_key(&Fault::unit_id).references(&Unit::id).on_delete.cascade()));
}
// clang-format on
using Db = decltype(make_db(""));

// What the dummy units can go wrong with.
struct Kind {
    int code;
    Severity severity;
    const char* text;
};
inline const std::vector<Kind>& kinds(const std::string& unit) {
    static const std::vector<Kind> eps{{101, warning, "battery below 7.0 V"},
                                       {102, info, "solar array current low"},
                                       {103, error, "battery heater stuck on"}};
    static const std::vector<Kind> adcs{{201, warning, "attitude error above 2 deg"},
                                        {202, error, "reaction wheel 2 not spinning"},
                                        {203, info, "star tracker blinded"}};
    static const std::vector<Kind> radio{{301, info, "frame with bad CRC"},
                                         {302, warning, "PA temperature above 70 C"},
                                         {303, error, "synthesizer lost lock"}};
    static const std::vector<Kind> other{{901, warning, "no heartbeat for 5 s"}, {902, error, "self test failed"}};
    if (unit == "eps") return eps;
    if (unit == "adcs") return adcs;
    if (unit == "radio") return radio;
    return other;
}

}  // namespace faults

class FaultLog : public forge::Application {
public:
    FaultLog() : Application({"fault-log", "1.0.0", "A fault log in SQLite, with sqlite_orm"}) {}

    // --- main loop only (tests use invoke()) ------------------------------------------

    /// Records a fault of `unit`; returns its id.
    std::int64_t raise(const std::string& unit, int code, int severity, const std::string& text) {
        faults::Fault f{
            .unit_id = unit_id(unit), .time_ms = wall_ms(), .code = code, .severity = severity, .text = text};
        f.id = db_->insert(f);
        ++raised_;
        last_ = std::format("#{} {} {}: {}", f.id, unit, faults::severity_name(severity), text);
        logger()->warn("fault {} ({} {}): {}", f.id, unit, code, text);
        trim();
        refresh();
        return f.id;
    }

    /// Clears one open fault; false if there is none of that id.
    bool clear(std::int64_t id) {
        using namespace sqlite_orm;
        db_->update_all(set(c(&faults::Fault::cleared_ms) = wall_ms()),
                        where(c(&faults::Fault::id) == id and is_null(&faults::Fault::cleared_ms)));
        const bool done = db_->changes() > 0;
        refresh();
        return done;
    }

    /// Clears every open fault of at least `severity`; returns how many.
    int clear_all(int severity = faults::info) {
        using namespace sqlite_orm;
        db_->update_all(set(c(&faults::Fault::cleared_ms) = wall_ms()),
                        where(is_null(&faults::Fault::cleared_ms) and c(&faults::Fault::severity) >= severity));
        const int n = db_->changes();
        refresh();
        return n;
    }

    /// The newest open faults, newest first.
    std::vector<faults::Fault> open_faults(int limit = 100) {
        using namespace sqlite_orm;
        return db_->get_all<faults::Fault>(where(is_null(&faults::Fault::cleared_ms)),
                                           order_by(&faults::Fault::id).desc(), sqlite_orm::limit(limit));
    }

    faults::Db& db() { return *db_; }

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("faults", "Fault log");
        s.string("faults.db", "faults.db", "The SQLite file");
        s.array("faults.units", forge::json::array({"eps", "adcs", "radio", "payload"}),
                "The units that report faults");
        s.integer("faults.period_ms", 1000, "How often the dummy source checks").range(1, 3600000).unit("ms");
        s.number("faults.rate", 0.2, "Chance of a fault at each check").range(0.0, 1.0);
        s.integer("faults.clear_after_s", 30, "Info and warnings clear themselves after").range(1, 86400).unit("s");
        s.integer("faults.keep", 10000, "Faults the log keeps; beyond, the oldest go").range(10, 10000000);
        s.integer("faults.seed", 1, "Seed of the dummy source (0: a different run each time)").range(0, 1 << 30);
    }

    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Faults");
        g.value("open", "Open", open_);
        g.value("open_errors", "Open errors", open_errors_);
        g.value("raised", "Raised (this run)", raised_);
        g.value("last", "Last", last_);
        g.button("clear_errors", "Clear all errors", [this] { clear_all(faults::error); }).danger();
    }

    void setup_console(forge::Console& console) override {
        console.add("faults", "faults [n]  the newest open faults (default 10)", [this, &console](const auto& args) {
            print(console, open_faults(args.empty() ? 10 : std::stoi(args[0])));
        });
        console.add("history", "history [unit] [n]  the newest faults, open or cleared",
                    [this, &console](const auto& args) {
            using namespace sqlite_orm;
            using faults::Fault;
            std::string unit;
            int n = 10;
            for (const auto& a : args) {  // the words after "history"
                if (!a.empty() && std::isdigit(static_cast<unsigned char>(a[0]))) {
                    n = std::stoi(a);
                } else {
                    unit = a;
                }
            }
            if (unit.empty()) {
                print(console, db_->get_all<Fault>(order_by(&Fault::id).desc(), limit(n)));
            } else {
                const auto ids = db_->select(&faults::Unit::id, where(c(&faults::Unit::name) == unit));
                if (ids.empty()) return console.print("no unit {}", unit);
                print(console,
                      db_->get_all<Fault>(where(c(&Fault::unit_id) == ids[0]), order_by(&Fault::id).desc(), limit(n)));
            }
        });
        console.add("stats", "stats  faults per unit, open ones per severity", [this, &console](const auto&) {
            using namespace sqlite_orm;
            using faults::Fault;
            using faults::Unit;
            // SELECT units.name, COUNT(faults.id) FROM units LEFT JOIN faults ON ... GROUP BY units.id
            const auto per_unit = db_->select(columns(&Unit::name, count(&Fault::id)),
                                              left_join<Fault>(on(c(&Fault::unit_id) == &Unit::id)),
                                              group_by(&Unit::id), order_by(&Unit::name));
            console.print("{:<10} {:>7}", "unit", "faults");
            for (const auto& [name, n] : per_unit) console.print("{:<10} {:>7}", name, n);
            const auto open = db_->select(columns(&Fault::severity, count(&Fault::id)),
                                          where(is_null(&Fault::cleared_ms)), group_by(&Fault::severity));
            for (const auto& [severity, n] : open) {
                console.print("open {:<8} {}", faults::severity_name(severity), n);
            }
        });
        console.add("clear", "clear <id> | all  clear a fault, or every open one", [this, &console](const auto& args) {
            if (args.empty()) return console.print("clear <id> | all");
            if (args[0] == "all") return console.print("{} cleared", clear_all());
            console.print("{}", clear(std::stoll(args[0])) ? "cleared" : "no open fault with that id");
        });
    }

    void on_start() override {
        const auto cfg = config();
        db_ = std::make_unique<faults::Db>(faults::make_db(cfg.value<std::string>("faults.db")));
        for (const auto& [table, change] : forge::orm::open(*db_)) {
            if (change != sqlite_orm::sync_schema_result::already_in_sync) {
                logger()->info("table {}: {}", table, (std::ostringstream() << change).str());
            }
        }
        units_ = cfg.value<std::vector<std::string>>("faults.units");
        for (const auto& u : units_) unit_id(u);  // known from the start: `stats` lists them all
        rate_ = cfg.value<double>("faults.rate");
        clear_after_ms_ = cfg.value<std::int64_t>("faults.clear_after_s") * 1000;
        keep_ = cfg.value<std::int64_t>("faults.keep");
        const auto seed = cfg.value<std::uint32_t>("faults.seed");
        random_.seed(seed ? seed : std::random_device{}());
        refresh();
        if (open_ > 0) logger()->info("{} faults still open from before", open_);

        every(std::chrono::milliseconds(cfg.value<int>("faults.period_ms")), [this] { check(); });
    }

private:
    // --- main loop ----------------------------------------------------------------------

    // The dummy source: now and then a fault; old info and warnings clear.
    void check() {
        using namespace sqlite_orm;
        using faults::Fault;
        db_->update_all(set(c(&Fault::cleared_ms) = wall_ms()),
                        where(is_null(&Fault::cleared_ms) and c(&Fault::severity) < int(faults::error) and
                              c(&Fault::time_ms) <= wall_ms() - clear_after_ms_));
        if (std::uniform_real_distribution<>(0, 1)(random_) < rate_ && !units_.empty()) {
            const auto& unit = units_[std::uniform_int_distribution<std::size_t>(0, units_.size() - 1)(random_)];
            const auto& choices = faults::kinds(unit);
            const auto& k = choices[std::uniform_int_distribution<std::size_t>(0, choices.size() - 1)(random_)];
            raise(unit, k.code, k.severity, k.text);
        } else {
            refresh();
        }
    }

    // The id of a unit, added on first use.
    std::int64_t unit_id(const std::string& name) {
        using namespace sqlite_orm;
        using faults::Unit;
        if (const auto ids = db_->select(&Unit::id, where(c(&Unit::name) == name)); !ids.empty()) return ids[0];
        return db_->insert(Unit{.name = name});
    }

    void trim() {
        using namespace sqlite_orm;
        using faults::Fault;
        const auto newest = db_->max(&Fault::id);
        if (newest && *newest > keep_) db_->remove_all<Fault>(where(c(&Fault::id) <= *newest - keep_));
    }

    // The panel's numbers.
    void refresh() {
        using namespace sqlite_orm;
        using faults::Fault;
        open_ = db_->count<Fault>(where(is_null(&Fault::cleared_ms)));
        open_errors_ =
            db_->count<Fault>(where(is_null(&Fault::cleared_ms) and c(&Fault::severity) == int(faults::error)));
    }

    void print(forge::Console& console, const std::vector<faults::Fault>& list) {
        console.print("{:>5}  {:>19}  {:<8} {:>5}  {:<8} {}", "id", "time (UTC)", "unit", "code", "severity", "text");
        for (const auto& f : list) {
            const auto unit = db_->get_pointer<faults::Unit>(f.unit_id);
            console.print("{:>5}  {:>19}  {:<8} {:>5}  {:<8} {}{}", f.id, utc(f.time_ms), unit ? unit->name : "?",
                          f.code, faults::severity_name(f.severity), f.text, f.cleared_ms ? "  (cleared)" : "");
        }
    }

    // Wall-clock time, so the times mean something in the file. Tests with a
    // manual clock move now() only: the offset follows it.
    std::int64_t wall_ms() const {
        using namespace std::chrono;
        if (!started_) {
            started_ = true;
            wall_at_start_ = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            now_at_start_ = now();
        }
        return wall_at_start_ + duration_cast<milliseconds>(now() - now_at_start_).count();
    }

    static std::string utc(std::int64_t ms) {
        return std::format("{:%Y-%m-%d %H:%M:%S}", std::chrono::sys_seconds(std::chrono::seconds(ms / 1000)));
    }

    std::unique_ptr<faults::Db> db_;  // main loop only: a storage is not thread safe
    std::vector<std::string> units_;
    double rate_ = 0.2;
    std::int64_t clear_after_ms_ = 30000, keep_ = 10000;
    std::mt19937 random_;

    mutable bool started_ = false;
    mutable std::int64_t wall_at_start_ = 0;
    mutable forge::Scheduler::Clock::time_point now_at_start_{};

    // The panel (bound values are read on the main loop).
    std::int64_t open_ = 0, open_errors_ = 0, raised_ = 0;
    std::string last_ = "none yet";
};

FORGE_MAIN(FaultLog)
