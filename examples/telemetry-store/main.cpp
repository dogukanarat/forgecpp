// telemetry-store: store and forward, as on a satellite between ground passes.
//
//   collect   main loop, every [telemetry].period_ms: a housekeeping record
//             (one JSON line) into the backlog, an SQLite file that survives
//             restarts and power cuts
//   downlink  a worker, while there is contact: the oldest records in
//             batches, compressed with LZ4, sent through a link of
//             [telemetry].downlink_bps (here: a file in [telemetry].outbox,
//             standing in for the radio); then they leave the backlog
//
// "Contact" (a control, or [telemetry].contact) starts and ends a pass.
// Without contact the backlog grows; when it is full the oldest records go
// and are counted. With LZ4 a pass carries about four times as many records.
//
// SQLite, twice in the same file:
//   - forge::Backlog keeps the records (its own table and connection);
//   - the pass log is plain SQL with forge::sqlite: every pass, its start
//     and end and what it carried, in the table "passes" (main loop only).
//     The console's `passes` lists them; any SQLite tool reads the file:
//       sqlite3 backlog.db "SELECT * FROM passes"
//
// The batches open with the standard tool:
//   lz4 -dc outbox/batch-000001.jsonl.lz4 | head
//
//   ./telemetry-store -c telemetry-store.toml --http 8080

#include <forge/backlog.hpp>
#include <forge/forge.hpp>
#include <forge/lz4.hpp>
#include <forge/sqlite.hpp>

#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>

using namespace std::chrono_literals;

class TelemetryStore : public forge::Application {
public:
    TelemetryStore() : Application({"telemetry-store", "1.0.0", "Store and forward with SQLite and LZ4"}) {}

    forge::Backlog& backlog() { return *backlog_; }
    const std::filesystem::path& outbox() const { return outbox_; }

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("telemetry", "Store and forward");
        s.string("telemetry.backlog", "backlog.db", "The backlog's SQLite file");
        s.integer("telemetry.max_records", 100000, "Records the backlog keeps; beyond, the oldest go")
            .range(10, 100000000);
        s.integer("telemetry.period_ms", 100, "A housekeeping record every").range(1, 3600000).unit("ms");
        s.string("telemetry.outbox", "outbox", "Where the downlink writes its batches (stands in for the radio)");
        s.integer("telemetry.batch_bytes", 16384, "Raw bytes in one batch, about").range(256, 16777216).unit("B");
        s.integer("telemetry.downlink_bps", 64000, "Link rate during a pass (0: no limit)")
            .range(0, 1000000000)
            .unit("bit/s");
        s.boolean("telemetry.contact", false, "Start with contact (a pass in progress)");
    }

    void setup_controls(forge::Controls& ui) override {
        auto& pass = ui.group("Pass");
        // A setter of its own: starting and ending a pass are written to the pass log.
        pass.toggle("contact", "Contact", [this] { return contact_.load(); }, [this](bool on) { set_contact(on); });
        pass.value("batches", "Batches sent", batches_);
        pass.value("records_sent", "Records sent", records_sent_);
        pass.value("ratio", "Compression", [this] {
            const double packed = static_cast<double>(packed_bytes_.load());
            return packed > 0 ? static_cast<double>(raw_bytes_.load()) / packed : 0.0;
        }).precision(2);

        auto& store = ui.group("Backlog");
        store.value("waiting", "Records waiting", [this] { return backlog_ ? backlog_->size() : 0; });
        store.value("waiting_kb", "Data waiting", [this] { return backlog_ ? backlog_->bytes() / 1024.0 : 0.0; })
            .unit("kB")
            .precision(1);
        store.value("dropped", "Dropped (full)", [this] { return backlog_ ? backlog_->dropped() : 0; });

        auto& log = ui.group("Pass log");
        log.value("passes", "Passes", passes_);
        log.value("last_pass", "Last pass", last_pass_);
    }

    void setup_console(forge::Console& console) override {
        // passes [n]: the newest passes, from the database (console commands run on the main loop).
        console.add("passes", "passes [n]  the last n ground passes (default 10)", [this, &console](const auto& args) {
            auto q =
                db_->prepare("SELECT id, start_ms, end_ms, records, packed_bytes FROM passes ORDER BY id DESC LIMIT ?");
            q.bind(1, args.empty() ? 10 : std::stoi(args[0]));  // args: after the command name
            console.print("{:>4}  {:>19}  {:>8}  {:>8}  {:>9}", "pass", "start (UTC)", "length", "records", "sent");
            while (q.step()) {
                const auto start = q.integer(1);
                const auto length =
                    q.null(2) ? std::string("open") : std::format("{} s", (q.integer(2) - start) / 1000);
                console.print("{:>4}  {:>19}  {:>8}  {:>8}  {:>7} kB", q.integer(0), utc(start), length, q.integer(3),
                              q.integer(4) / 1024);
            }
        });
    }

    void on_start() override {
        const auto cfg = config();
        backlog_ = std::make_unique<forge::Backlog>(
            cfg.value<std::string>("telemetry.backlog"),
            forge::BacklogOptions{.max_records = cfg.value<std::size_t>("telemetry.max_records")});
        outbox_ = cfg.value<std::string>("telemetry.outbox");
        std::filesystem::create_directories(outbox_);
        batch_bytes_ = cfg.value<std::size_t>("telemetry.batch_bytes");
        downlink_bps_ = cfg.value<std::int64_t>("telemetry.downlink_bps");
        if (backlog_->size() > 0) logger()->info("backlog: {} records from before", backlog_->size());

        // The pass log: a second connection to the same file (WAL lets the
        // backlog's worker write meanwhile), used on the main loop only.
        db_ = std::make_unique<forge::sqlite::Database>(cfg.value<std::string>("telemetry.backlog"));
        db_->exec(
            "CREATE TABLE IF NOT EXISTS passes ("
            " id INTEGER PRIMARY KEY, start_ms INTEGER NOT NULL, end_ms INTEGER,"
            " records INTEGER NOT NULL DEFAULT 0, packed_bytes INTEGER NOT NULL DEFAULT 0)");
        // A pass the program did not see end (a crash, a power cut): closed where it started.
        db_->exec("UPDATE passes SET end_ms = start_ms WHERE end_ms IS NULL");
        auto count = db_->prepare("SELECT COUNT(*) FROM passes");
        if (count.step()) passes_ = count.integer(0);
        if (cfg.value<bool>("telemetry.contact")) set_contact(true);

        every(std::chrono::milliseconds(cfg.value<int>("telemetry.period_ms")), [this] { collect(); });
        worker("downlink", [this](std::stop_token stop) { downlink(stop); });
    }

    void on_stop() override {
        if (contact_.load()) end_pass();  // the downlink has stopped: the pass ends here
    }

private:
    // --- main loop: the pass log (forge::sqlite) ----------------------------------------
    void set_contact(bool on) {
        if (on == contact_.load()) return;
        if (on) {
            start_pass();
            contact_ = true;  // the downlink may go
        } else {
            contact_ = false;
            end_pass();
        }
    }

    void start_pass() {
        db_->prepare("INSERT INTO passes (start_ms) VALUES (?)").bind(1, now_ms()).run();
        pass_id_ = db_->last_insert_id();
        records_at_start_ = records_sent_.load();
        packed_at_start_ = packed_bytes_.load();
        logger()->info("pass {} begins", pass_id_);
    }

    void end_pass() {
        const auto records = static_cast<std::int64_t>(records_sent_.load() - records_at_start_);
        const auto packed = static_cast<std::int64_t>(packed_bytes_.load() - packed_at_start_);
        // Both or neither: the pass is closed, and the log keeps its newest 1000 passes.
        forge::sqlite::Transaction tx(*db_);
        db_->prepare("UPDATE passes SET end_ms = ?, records = ?, packed_bytes = ? WHERE id = ?")
            .bind(1, now_ms())
            .bind(2, records)
            .bind(3, packed)
            .bind(4, pass_id_)
            .run();
        db_->prepare("DELETE FROM passes WHERE id <= ?").bind(1, pass_id_ - 1000).run();
        tx.commit();
        ++passes_;
        last_pass_ = std::format("#{}: {} records, {} kB", pass_id_, records, packed / 1024);
        logger()->info("pass {} ends: {} records, {} bytes sent", pass_id_, records, packed);
    }

    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static std::string utc(std::int64_t ms) {
        const auto t = std::chrono::sys_seconds(std::chrono::seconds(ms / 1000));
        return std::format("{:%Y-%m-%d %H:%M:%S}", t);
    }

    // --- main loop: one housekeeping record -------------------------------------------
    void collect() {
        ++seq_;
        const double t = static_cast<double>(seq_) / 10.0;
        // A dummy satellite: battery, temperatures, attitude error.
        const auto record = std::format(
            R"({{"seq":{},"battery_v":{:.3f},"obc_temp_c":{:.2f},"panel_temp_c":{:.2f},"att_err_deg":{:.4f},"mode":"nominal"}})",
            seq_, 7.8 + 0.2 * std::sin(t / 50), 21 + 3 * std::sin(t / 30), -40 + 80 * std::sin(t / 60),
            0.01 * std::cos(t));
        backlog_->append("hk", record);
    }

    // --- worker: the pass ---------------------------------------------------------------
    void downlink(const std::stop_token& stop) {
        auto last_sent = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            if (!contact_.load()) {
                forge::sleep_for(stop, 200ms);
                continue;
            }
            // Less than a batch waiting (the backlog is drained, records come
            // one by one): wait a second for more. A batch of one record
            // compresses badly and costs a file each.
            if (backlog_->bytes() < batch_bytes_ && std::chrono::steady_clock::now() - last_sent < 1s) {
                forge::sleep_for(stop, 100ms);
                continue;
            }
            const auto batch = backlog_->peek(100000, batch_bytes_);
            if (batch.empty()) {
                forge::sleep_for(stop, 100ms);
                continue;
            }
            last_sent = std::chrono::steady_clock::now();
            std::string lines;
            for (const auto& r : batch) {
                lines.append(r.data.begin(), r.data.end());
                lines += '\n';
            }
            const auto packed = forge::lz4::compress(lines);

            // The radio: as long as the bytes take at this rate (the pass may end meanwhile).
            if (downlink_bps_ > 0) {
                const auto airtime =
                    std::chrono::microseconds(static_cast<std::int64_t>(packed.size()) * 8 * 1'000'000 / downlink_bps_);
                if (forge::sleep_for(stop, airtime) || !contact_.load()) continue;  // not sent: the records stay
            }
            const auto n = batches_.fetch_add(1) + 1;
            const auto file = outbox_ / std::format("batch-{:06}.jsonl.lz4", n);
            {
                std::ofstream out(file.string() + ".part", std::ios::binary);
                out.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
            }
            std::filesystem::rename(file.string() + ".part", file);

            backlog_->remove_through(batch.back().id);  // delivered: they leave the backlog
            records_sent_.fetch_add(batch.size());
            raw_bytes_.fetch_add(lines.size());
            packed_bytes_.fetch_add(packed.size());
        }
    }

    std::unique_ptr<forge::Backlog> backlog_;  // thread safe
    std::unique_ptr<forge::sqlite::Database> db_;  // the pass log: main loop only
    std::int64_t pass_id_ = 0, passes_ = 0;
    std::uint64_t records_at_start_ = 0, packed_at_start_ = 0;
    std::string last_pass_ = "none yet";
    std::filesystem::path outbox_;
    std::size_t batch_bytes_ = 16384;
    std::int64_t downlink_bps_ = 64000;
    std::uint64_t seq_ = 0;  // main loop

    std::atomic<bool> contact_{false};
    std::atomic<std::uint64_t> batches_{0}, records_sent_{0}, raw_bytes_{0}, packed_bytes_{0};
};

FORGE_MAIN(TelemetryStore)
