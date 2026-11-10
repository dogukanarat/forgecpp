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
// The batches open with the standard tool:
//   lz4 -dc outbox/batch-000001.jsonl.lz4 | head
//
//   ./telemetry-store -c telemetry-store.toml --http 8080

#include <forge/backlog.hpp>
#include <forge/forge.hpp>
#include <forge/lz4.hpp>

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
        pass.toggle("contact", "Contact", contact_);
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
        contact_ = cfg.value<bool>("telemetry.contact");
        if (backlog_->size() > 0) logger()->info("backlog: {} records from before", backlog_->size());

        every(std::chrono::milliseconds(cfg.value<int>("telemetry.period_ms")), [this] { collect(); });
        worker("downlink", [this](std::stop_token stop) { downlink(stop); });
    }

private:
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
    std::filesystem::path outbox_;
    std::size_t batch_bytes_ = 16384;
    std::int64_t downlink_bps_ = 64000;
    std::uint64_t seq_ = 0;  // main loop

    std::atomic<bool> contact_{false};
    std::atomic<std::uint64_t> batches_{0}, records_sent_{0}, raw_bytes_{0}, packed_bytes_{0};
};

FORGE_MAIN(TelemetryStore)
