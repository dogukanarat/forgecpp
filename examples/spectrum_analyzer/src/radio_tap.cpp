#include "radio_tap.hpp"

#include <forge/zmq.hpp>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

namespace sa {
namespace {

using radio::DeviceError;
namespace fz = forge::zmq;

class RadioTapBackend final : public radio::Backend, public FixedTuning {
public:
    explicit RadioTapBackend(RadioTapOptions o) : options_(std::move(o)), control_(options_.control, options_.timeout) {
        // Check radio-controller first: a clear error beats a silent wait for data.
        try {
            const auto channels = call("get", {{"id", "rx_channels"}})["value"];
            if (channels.is_number() && channels.get<int>() < 2) {
                throw DeviceError("radio-controller's transceiver has one RX channel (AD9364): no RX2 to analyze");
            }
            if (options_.enable_rx2 && call("get", {{"id", "rx2_enabled"}})["value"] != true) {
                call("set", {{"id", "rx2_enabled"}, {"value", true}});
            }
        } catch (const fz::Error& e) {
            throw DeviceError("radio-controller at " + options_.control + ": " + e.what());
        }
        fz::Receiver::Options ro;
        ro.topics = {"iq"};
        ro.socket.recv_hwm = 4;
        receiver_ = std::make_unique<fz::Receiver>(
            fz::Receiver::Kind::sub, options_.tap, [this](fz::Message&& m) { on_block(std::move(m)); }, ro);
    }

    ~RadioTapBackend() override {
        cancel_rx();
        receiver_.reset();
    }

    std::string name() const override { return "radio-controller RX2 (" + options_.tap + ")"; }

    nlohmann::json describe() const override {
        return {{"backend", "radio"}, {"tap", options_.tap}, {"control", options_.control}, {"channel", 2}};
    }

    // --- tuning belongs to radio-controller -------------------------------------
    void set_rx_lo(double) override { not_here("the RX LO"); }
    void set_tx_lo(double) override { not_here("the TX LO"); }
    void set_sample_rate(double) override { not_here("the sample rate"); }
    void set_rx_bandwidth(double) override { not_here("the RX bandwidth"); }
    void set_tx_bandwidth(double) override { not_here("the TX bandwidth"); }
    void set_tx_attenuation(double) override { not_here("TX attenuation"); }

    // --- RX2 gain through the control socket ---------------------------------------
    void set_gain_mode(const std::string& mode, unsigned ch) override {
        check(ch);
        remote_set("rx2_gain_mode", mode);
    }
    void set_rx_gain(double db, unsigned ch) override {
        check(ch);
        remote_set("rx2_gain_db", db);
        gain_db_ = db;
    }
    double rx_gain(unsigned ch) override {
        check(ch);
        return gain_db_;
    }
    std::optional<double> temperature_c() override {
        try {
            const auto v = call("get", {{"id", "temperature_c"}})["value"];
            if (v.is_number()) return v.get<double>();
        } catch (const fz::Error&) {
        }
        return std::nullopt;
    }
    unsigned rx_channel_count() const override { return 1; }  // RX2 of the radio, seen as our only channel

    // --- data ----------------------------------------------------------------------
    void start_rx(std::size_t, unsigned channels) override {
        if (channels > 1) throw DeviceError("the radio-controller tap carries RX2 only");
        cancelled_ = false;
    }

    std::size_t read_rx(std::int16_t* rx1, std::int16_t*, std::size_t max_samples) override {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, options_.timeout, [this] { return cancelled_ || fresh_; })) {
            throw DeviceError(no_data());
        }
        if (cancelled_) return 0;
        fresh_ = false;
        const std::size_t n = std::min(max_samples, block_.size() / 2);
        if (rx1) std::memcpy(rx1, block_.data(), n * 2 * sizeof(std::int16_t));
        last_read_ = block_tuning_;
        return n;
    }

    void stop_rx() override {}
    void cancel_rx() override {
        {
            std::lock_guard lock(mutex_);
            cancelled_ = true;
        }
        cv_.notify_all();
    }
    bool take_rx_overflow() override { return false; }

    void start_tx(std::size_t, bool) override { not_here("TX"); }
    void write_tx(const std::int16_t*, std::size_t) override { not_here("TX"); }
    void stop_tx() override {}
    bool take_tx_underflow() override { return false; }

    // --- FixedTuning -------------------------------------------------------------------
    Tuning tuning() override {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, options_.timeout, [this] { return cancelled_ || have_block_; })) {
            throw DeviceError(no_data());
        }
        return block_tuning_;
    }
    Tuning last_read() const override {
        std::lock_guard lock(mutex_);
        return last_read_;
    }

private:
    [[noreturn]] void not_here(const char* what) const {
        throw DeviceError(std::string(what) + " is set in radio-controller (it owns the transceiver)");
    }
    void check(unsigned ch) const {
        if (ch != 0) throw DeviceError("the radio-controller tap carries RX2 only");
    }
    std::string no_data() const {
        return "no RX2 data from radio-controller at " + options_.tap + " for " +
               std::to_string(options_.timeout.count()) + " ms (is RX streaming on?)";
    }

    forge::json call(const std::string& cmd, const forge::json& args) { return control_.call(cmd, args); }

    template <class T>
    void remote_set(const char* id, const T& value) {
        try {
            call("set", {{"id", id}, {"value", value}});
        } catch (const fz::Error& e) {
            throw DeviceError(std::string("radio-controller: ") + e.what());
        }
    }

    // Receiver thread: keep only the newest block.
    void on_block(fz::Message&& m) {
        const auto& h = m.header;
        const std::size_t samples = h.value("samples", std::size_t{0});
        if (h.value("format", std::string()) != "cs16" || samples == 0 || m.payload.size() != samples * 4) return;
        Tuning t{h.value("center_freq", 0.0), h.value("sample_rate", 0.0), samples};
        if (t.rate_hz <= 0) return;
        {
            std::lock_guard lock(mutex_);
            block_.resize(2 * samples);
            std::memcpy(block_.data(), m.payload.data(), m.payload.size());
            block_tuning_ = t;
            have_block_ = fresh_ = true;
            if (h.contains("rx_gain_db") && h["rx_gain_db"].is_number()) gain_db_ = h["rx_gain_db"].get<double>();
        }
        cv_.notify_all();
    }

    RadioTapOptions options_;
    fz::Client control_;  // engine thread only
    std::unique_ptr<fz::Receiver> receiver_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::int16_t> block_;
    Tuning block_tuning_, last_read_;
    bool have_block_ = false, fresh_ = false, cancelled_ = false;
    std::atomic<double> gain_db_{0};
};

}  // namespace

std::unique_ptr<radio::Backend> make_radio_tap_backend(const RadioTapOptions& options) {
    return std::make_unique<RadioTapBackend>(options);
}

}  // namespace sa
