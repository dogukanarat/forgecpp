#include "radio_tap.hpp"

#include <forge/error.hpp>
#include <forge/remote.hpp>
#include <forge/zmq.hpp>

#include "radio/radio.forge.hpp"

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
        if (options_.channel < 1 || options_.channel > 2) throw DeviceError("channel must be 1 or 2");
        // radio-controller's control ids for the channel's gain
        mode_id_ = options_.channel == 1 ? "gain_mode" : "rx2_gain_mode";
        gain_id_ = options_.channel == 1 ? "rx_gain_db" : "rx2_gain_db";
        // Check radio-controller first: a clear error beats a silent wait for data.
        try {
            const auto shared = control_.get("tap_channels");
            bool found = false;
            for (const auto& c : shared) found = found || (c.is_number() && c.get<unsigned>() == options_.channel);
            if (!found) {
                const auto ch = std::to_string(options_.channel);
                throw DeviceError("radio-controller does not share RX" + ch + " (it shares " + shared.dump() +
                                  "): add " + ch + " to [radio] tap_rx in its configuration");
            }
        } catch (const forge::Error& e) {
            throw DeviceError("radio-controller at " + options_.control + ": " + e.what());
        }
        fz::Receiver::Options ro;
        ro.topics = {"rx" + std::to_string(options_.channel)};
        ro.socket.recv_hwm = 4;
        receiver_ = std::make_unique<fz::Receiver>(fz::Receiver::Kind::sub, options_.data,
                                                   [this](fz::Message&& m) { on_block(std::move(m)); }, ro);
    }

    ~RadioTapBackend() override {
        cancel_rx();
        receiver_.reset();
    }

    std::string name() const override {
        return "radio-controller RX" + std::to_string(options_.channel) + " (" + options_.data + ")";
    }

    nlohmann::json describe() const override {
        return {{"backend", "radio"},
                {"data", options_.data},
                {"control", options_.control},
                {"channel", options_.channel},
                {"control_gain", options_.control_gain}};
    }

    // --- tuning belongs to radio-controller -------------------------------------
    void set_rx_lo(double) override { not_here("the RX LO"); }
    void set_tx_lo(double) override { not_here("the TX LO"); }
    void set_sample_rate(double) override { not_here("the sample rate"); }
    void set_rx_bandwidth(double) override { not_here("the RX bandwidth"); }
    void set_tx_bandwidth(double) override { not_here("the TX bandwidth"); }
    void set_tx_attenuation(double, unsigned) override { not_here("TX attenuation"); }

    // --- gain of the channel, through the control socket (if ours to set) ------------
    void set_gain_mode(const std::string& mode, unsigned ch) override {
        check(ch);
        if (options_.control_gain) remote_set(mode_id_, mode);
    }
    void set_rx_gain(double db, unsigned ch) override {
        check(ch);
        if (!options_.control_gain) return;
        remote_set(gain_id_, db);
        gain_db_ = db;
    }
    double rx_gain(unsigned ch) override {
        check(ch);
        return gain_db_;
    }
    std::optional<double> temperature_c() override {
        try {
            const auto v = control_.get("temperature_c");
            if (v.is_number()) return v.get<double>();
        } catch (const forge::Error&) {
        }
        return std::nullopt;
    }
    unsigned rx_channel_count() const override { return 1; }  // the shared channel, seen as our only one
    unsigned tx_channel_count() const override { return 0; }

    // --- data ----------------------------------------------------------------------
    void start_rx(std::size_t, unsigned channels) override {
        if (channels != kRx1) throw DeviceError("the radio-controller tap carries one channel");
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

    void start_tx(std::size_t, bool, unsigned) override { not_here("TX"); }
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
        if (ch != 0) throw DeviceError("the radio-controller tap carries one channel");
    }
    std::string no_data() const {
        return "no RX" + std::to_string(options_.channel) + " data from radio-controller at " + options_.data +
               " for " + std::to_string(options_.timeout.count()) + " ms (is RX streaming on?)";
    }

    template <class T>
    void remote_set(const std::string& id, const T& value) {
        try {
            control_.set(id, value);
        } catch (const forge::Error& e) {
            throw DeviceError(std::string("radio-controller: ") + e.what());
        }
    }

    // Receiver thread: keep only the newest block.
    void on_block(fz::Message&& m) {
        radio_iq_block h{};
        try {
            h = m.as<radio_iq_block>();
        } catch (const fz::Error&) {
            return;  // not an IQ block
        }
        const std::size_t samples = h.samples;
        if (std::string_view(h.format) != "cs16" || samples == 0 || m.blob.size() != samples * 4) return;
        Tuning t{h.center_freq, h.sample_rate, samples};
        if (t.rate_hz <= 0) return;
        {
            std::lock_guard lock(mutex_);
            block_.resize(2 * samples);
            std::memcpy(block_.data(), m.blob.data(), m.blob.size());
            block_tuning_ = t;
            have_block_ = fresh_ = true;
            gain_db_ = h.rx_gain_db;
        }
        cv_.notify_all();
    }

    RadioTapOptions options_;
    std::string mode_id_, gain_id_;  ///< radio-controller control ids of the channel's gain
    forge::Remote control_;  // engine thread only
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
