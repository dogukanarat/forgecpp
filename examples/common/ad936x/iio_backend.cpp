// AD936x backend on libiio (0.x API).
//
// Devices of the ADI ad9361 driver (used for AD9361/AD9363/AD9364):
//   ad9361-phy               transceiver control
//     altvoltage0 (out)        RX LO: frequency
//     altvoltage1 (out)        TX LO: frequency
//     voltage0 (in)            RX1: sampling_frequency, rf_bandwidth, gain_control_mode, hardwaregain
//     voltage1 (in)            RX2: gain_control_mode, hardwaregain (AD9361/AD9363 only)
//     voltage0 (out)           TX1: rf_bandwidth, hardwaregain (= -attenuation)
//     voltage1 (out)           TX2: hardwaregain (AD9361/AD9363 only)
//     temp0 (in)               input (milli-degree C)
//   cf-ad9361-lpc            RX DMA (voltage0/1 = RX1 I/Q, voltage2/3 = RX2 I/Q on 2R parts)
//                            (FMCOMMS5: cf-ad9361-A, chip A on voltage0-3)
//   cf-ad9361-dds-core-lpc   TX DMA (voltage0/1 = TX1 I/Q, voltage2/3 = TX2 I/Q on 2T parts)
//
// Over/underflows are read from the AXI AD9361 core status register 0x80000088
// (bit 2: ADC overflow, bit 0: DAC underflow), as ADI's iio_readdev/iio_writedev do.

#include "backend.hpp"

#include <iio.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <mutex>

namespace radio {
namespace {

constexpr std::uint32_t kStatusRegister = 0x80000088;
constexpr std::uint32_t kRxOverflow = 1u << 2;
constexpr std::uint32_t kTxUnderflow = 1u << 0;

std::string errstr(int err) {
    char buf[256];
    iio_strerror(err < 0 ? -err : err, buf, sizeof(buf));
    return buf;
}

[[noreturn]] void fail(const std::string& what, int err) { throw DeviceError(what + ": " + errstr(err)); }

class IioBackend final : public Backend {
public:
    IioBackend(const std::string& uri, unsigned timeout_ms) : uri_(uri) {
        ctx_ = uri.empty() ? iio_create_default_context() : iio_create_context_from_uri(uri.c_str());
        if (!ctx_) fail("cannot open IIO context '" + (uri.empty() ? std::string("default") : uri) + "'", errno);
        // Not every backend has a timeout (e.g. xml:): ENOSYS is fine.
        if (int r = iio_context_set_timeout(ctx_, timeout_ms); r < 0 && r != -ENOSYS) {
            iio_context_destroy(ctx_);
            fail("cannot set IIO timeout", r);
        }

        phy_ = iio_context_find_device(ctx_, "ad9361-phy");
        rx_dev_ = iio_context_find_device(ctx_, "cf-ad9361-lpc");
        // FMCOMMS5 (two AD9361 chips, 4R4T): the synchronized RX DMA of both chips is
        // "cf-ad9361-A" (voltage0-3 = chip A RX1/RX2, voltage4-7 = chip B); TX stays
        // "cf-ad9361-dds-core-lpc". This backend drives chip A (ad9361-phy).
        if (!rx_dev_) rx_dev_ = iio_context_find_device(ctx_, "cf-ad9361-A");
        tx_dev_ = iio_context_find_device(ctx_, "cf-ad9361-dds-core-lpc");
        if (!phy_ || !rx_dev_ || !tx_dev_) {
            std::string found;
            for (unsigned i = 0; i < iio_context_get_devices_count(ctx_); ++i) {
                const char* n = iio_device_get_name(iio_context_get_device(ctx_, i));
                found += std::string(found.empty() ? "" : ", ") + (n ? n : "?");
            }
            iio_context_destroy(ctx_);
            throw DeviceError("no AD936x found at '" + uri + "' (devices: " + (found.empty() ? "none" : found) + ")");
        }

        rx_lo_ = channel(phy_, "altvoltage0", true);
        tx_lo_ = channel(phy_, "altvoltage1", true);
        rx_ = channel(phy_, "voltage0", false);
        tx_ = channel(phy_, "voltage0", true);
        temp_ = iio_device_find_channel(phy_, "temp0", false);  // optional
        rx_i_ = channel(rx_dev_, "voltage0", false);
        rx_q_ = channel(rx_dev_, "voltage1", false);
        // RX2 exists on AD9361/AD9363 in 2R2T mode (optional)
        rx2_i_ = iio_device_find_channel(rx_dev_, "voltage2", false);
        rx2_q_ = iio_device_find_channel(rx_dev_, "voltage3", false);
        rx2_ = iio_device_find_channel(phy_, "voltage1", false);
        if (!rx2_i_ || !rx2_q_ || !rx2_) rx2_i_ = rx2_q_ = rx2_ = nullptr;
        tx2_i_ = iio_device_find_channel(tx_dev_, "voltage2", true);
        tx2_q_ = iio_device_find_channel(tx_dev_, "voltage3", true);
        tx2_ = iio_device_find_channel(phy_, "voltage1", true);
        if (!tx2_i_ || !tx2_q_ || !tx2_) tx2_i_ = tx2_q_ = tx2_ = nullptr;
        tx_i_ = channel(tx_dev_, "voltage0", true);
        tx_q_ = channel(tx_dev_, "voltage1", true);

        char mode[64] = {};
        if (iio_channel_attr_read(rx_, "gain_control_mode", mode, sizeof(mode)) > 0) gain_mode_[0] = mode;
        if (rx2_ && iio_channel_attr_read(rx2_, "gain_control_mode", mode, sizeof(mode)) > 0) gain_mode_[1] = mode;
    }

    ~IioBackend() override {
        stop_rx();
        stop_tx();
        iio_context_destroy(ctx_);
    }

    std::string name() const override { return "iio:" + (uri_.empty() ? std::string("default") : uri_); }

    nlohmann::json describe() const override {
        nlohmann::json j{{"backend", "iio"},
                         {"uri", uri_},
                         {"rx_channels", rx_channel_count()},
                         {"tx_channels", tx_channel_count()}};
        const char* desc = iio_context_get_description(ctx_);
        if (desc) j["description"] = desc;
        for (unsigned i = 0; i < iio_context_get_attrs_count(ctx_); ++i) {
            const char *key = nullptr, *value = nullptr;
            if (iio_context_get_attr(ctx_, i, &key, &value) == 0 && key && value) j["attrs"][key] = value;
        }
        unsigned major = 0, minor = 0;
        char tag[8] = {};
        iio_library_get_version(&major, &minor, tag);
        j["libiio"] = std::to_string(major) + "." + std::to_string(minor) + " (" + tag + ")";
        return j;
    }

    // --- control -------------------------------------------------------------
    void set_rx_lo(double hz) override { write_ll(rx_lo_, "frequency", hz, "RX LO"); }
    void set_tx_lo(double hz) override { write_ll(tx_lo_, "frequency", hz, "TX LO"); }
    void set_sample_rate(double hz) override { write_ll(rx_, "sampling_frequency", hz, "sample rate"); }
    void set_rx_bandwidth(double hz) override { write_ll(rx_, "rf_bandwidth", hz, "RX bandwidth"); }
    void set_tx_bandwidth(double hz) override { write_ll(tx_, "rf_bandwidth", hz, "TX bandwidth"); }

    void set_gain_mode(const std::string& mode, unsigned ch) override {
        std::lock_guard lock(ctrl_mutex_);
        if (const auto r = iio_channel_attr_write(rx_phy(ch), "gain_control_mode", mode.c_str()); r < 0) {
            fail("cannot set RX" + std::to_string(ch + 1) + " gain mode '" + mode + "'", static_cast<int>(r));
        }
        gain_mode_[ch] = mode;
    }

    void set_rx_gain(double db, unsigned ch) override {
        std::lock_guard lock(ctrl_mutex_);
        auto* c = rx_phy(ch);
        if (gain_mode_[ch] != "manual") throw DeviceError("RX gain can only be set in manual gain mode");
        if (int r = iio_channel_attr_write_double(c, "hardwaregain", db); r < 0) {
            fail("cannot set RX" + std::to_string(ch + 1) + " gain to " + std::to_string(db) + " dB", r);
        }
    }

    void set_tx_attenuation(double db, unsigned ch) override {
        std::lock_guard lock(ctrl_mutex_);
        const double quantized = std::round(db * 4) / 4;  // 0.25 dB steps
        if (int r = iio_channel_attr_write_double(tx_phy(ch), "hardwaregain", -quantized); r < 0) {
            fail("cannot set TX" + std::to_string(ch + 1) + " attenuation to " + std::to_string(db) + " dB", r);
        }
    }

    double rx_gain(unsigned ch) override {
        std::lock_guard lock(ctrl_mutex_);
        double v = 0;
        if (int r = iio_channel_attr_read_double(rx_phy(ch), "hardwaregain", &v); r < 0) fail("cannot read RX gain", r);
        return v;
    }

    unsigned rx_channel_count() const override { return rx2_ ? 2 : 1; }
    unsigned tx_channel_count() const override { return tx2_ ? 2 : 1; }

    std::optional<double> temperature_c() override {
        if (!temp_) return std::nullopt;
        std::lock_guard lock(ctrl_mutex_);
        double v = 0;
        if (iio_channel_attr_read_double(temp_, "input", &v) < 0) return std::nullopt;
        return v / 1000.0;
    }

    // --- RX ------------------------------------------------------------------------
    void start_rx(std::size_t samples, unsigned mask) override {
        stop_rx();
        if ((mask & kRx2) && !rx2_i_) throw DeviceError(no_second_channel("RX"));
        if ((mask & (kRx1 | kRx2)) == 0) throw DeviceError("no RX channel selected");
        enable(rx_i_, rx_q_, mask & kRx1);
        if (rx2_i_) enable(rx2_i_, rx2_q_, mask & kRx2);
        rx_mask_ = mask;
        auto* buf = iio_device_create_buffer(rx_dev_, samples, false);
        if (!buf) fail("cannot create RX buffer of " + std::to_string(samples) + " samples", errno);
        const auto* fmt = iio_channel_get_data_format((mask & kRx1) ? rx_i_ : rx2_i_);
        rx_bits_ = fmt->bits;
        rx_shift_ = fmt->shift;
        clear_status(rx_dev_);
        cancelled_ = false;
        std::lock_guard lock(rx_mutex_);
        rx_buf_ = buf;
    }

    std::size_t read_rx(std::int16_t* rx1, std::int16_t* rx2, std::size_t max_samples) override {
        iio_buffer* buf;
        {
            std::lock_guard lock(rx_mutex_);
            buf = rx_buf_;
        }
        if (!buf || cancelled_) return 0;
        const auto r = iio_buffer_refill(buf);
        if (r < 0) {
            if (cancelled_) return 0;
            fail("RX buffer refill failed", static_cast<int>(r));
        }
        check_status(rx_dev_, kRxOverflow, rx_overflow_, rx_status_ok_);

        // One sample of every enabled channel per step: [I1 Q1] [I2 Q2].
        const auto step = iio_buffer_step(buf);
        const auto* first = static_cast<const std::uint8_t*>(iio_buffer_first(buf, (rx_mask_ & kRx1) ? rx_i_ : rx2_i_));
        const auto* end = static_cast<const std::uint8_t*>(iio_buffer_end(buf));
        const std::size_t available =
            step > 0 ? static_cast<std::size_t>(end - first) / static_cast<std::size_t>(step) : 0;
        const std::size_t n = std::min(available, max_samples);
        auto convert = [&](const std::uint8_t* p, std::int16_t* out) {
            for (std::size_t i = 0; i < n; ++i, p += step) {
                const auto* s = reinterpret_cast<const std::int16_t*>(p);
                out[2 * i] = to_full_scale(s[0], rx_bits_, rx_shift_);
                out[2 * i + 1] = to_full_scale(s[1], rx_bits_, rx_shift_);
            }
        };
        if (rx1 && (rx_mask_ & kRx1)) convert(first, rx1);
        if (rx2 && (rx_mask_ & kRx2)) convert(static_cast<const std::uint8_t*>(iio_buffer_first(buf, rx2_i_)), rx2);
        return n;
    }

    void stop_rx() override {
        std::lock_guard lock(rx_mutex_);
        if (rx_buf_) {
            iio_buffer_destroy(rx_buf_);
            rx_buf_ = nullptr;
        }
    }

    void cancel_rx() override {
        cancelled_ = true;
        std::lock_guard lock(rx_mutex_);
        if (rx_buf_) iio_buffer_cancel(rx_buf_);
    }

    bool take_rx_overflow() override { return rx_overflow_.exchange(false); }

    // --- TX ------------------------------------------------------------------------
    void start_tx(std::size_t samples, bool cyclic, unsigned ch) override {
        std::lock_guard lock(tx_mutex_);
        destroy_tx_locked();
        if (ch > 1 || (ch == 1 && !tx2_i_)) throw DeviceError(no_second_channel("TX"));
        enable(tx_i_, tx_q_, ch == 0);
        if (tx2_i_) enable(tx2_i_, tx2_q_, ch == 1);
        tx_ch_i_ = ch == 0 ? tx_i_ : tx2_i_;
        tx_buf_ = iio_device_create_buffer(tx_dev_, samples, cyclic);
        if (!tx_buf_) fail("cannot create TX buffer of " + std::to_string(samples) + " samples", errno);
        const auto* fmt = iio_channel_get_data_format(tx_ch_i_);
        tx_bits_ = fmt->bits;
        tx_shift_ = fmt->shift;
        tx_samples_ = samples;
        clear_status(tx_dev_);
    }

    void write_tx(const std::int16_t* iq, std::size_t samples) override {
        std::lock_guard lock(tx_mutex_);
        if (!tx_buf_) throw DeviceError("TX is not started");
        const auto step = iio_buffer_step(tx_buf_);
        auto* p = static_cast<std::uint8_t*>(iio_buffer_first(tx_buf_, tx_ch_i_));
        auto* end = static_cast<std::uint8_t*>(iio_buffer_end(tx_buf_));
        std::size_t n = 0;
        for (; p < end; p += step, ++n) {
            auto* s = reinterpret_cast<std::int16_t*>(p);
            const bool have = n < samples;
            s[0] = have ? from_full_scale(iq[2 * n], tx_bits_, tx_shift_) : 0;
            s[1] = have ? from_full_scale(iq[2 * n + 1], tx_bits_, tx_shift_) : 0;
        }
        if (const auto r = iio_buffer_push(tx_buf_); r < 0) fail("TX buffer push failed", static_cast<int>(r));
        check_status(tx_dev_, kTxUnderflow, tx_underflow_, tx_status_ok_);
    }

    void stop_tx() override {
        std::lock_guard lock(tx_mutex_);
        destroy_tx_locked();
    }

    bool take_tx_underflow() override { return tx_underflow_.exchange(false); }

private:
    static std::string no_second_channel(const std::string& dir) {
        return "this transceiver has one " + dir + " channel (AD9364, or an AD9361 in 1R1T mode); " + dir +
               "2 needs an AD9361/AD9363 in 2R2T mode";
    }

    static void enable(iio_channel* i, iio_channel* q, bool on) {
        if (on) {
            iio_channel_enable(i);
            iio_channel_enable(q);
        } else {
            iio_channel_disable(i);
            iio_channel_disable(q);
        }
    }

    iio_channel* rx_phy(unsigned ch) {
        if (ch == 0) return rx_;
        if (ch == 1 && rx2_) return rx2_;
        throw DeviceError(no_second_channel("RX"));
    }

    iio_channel* tx_phy(unsigned ch) {
        if (ch == 0) return tx_;
        if (ch == 1 && tx2_) return tx2_;
        throw DeviceError(no_second_channel("TX"));
    }

    iio_channel* channel(iio_device* dev, const char* id, bool output) {
        auto* ch = iio_device_find_channel(dev, id, output);
        if (!ch) {
            throw DeviceError(std::string("channel ") + id + (output ? " (output)" : " (input)") + " not found on " +
                              iio_device_get_name(dev));
        }
        return ch;
    }

    void write_ll(iio_channel* ch, const char* attr, double value, const char* what) {
        std::lock_guard lock(ctrl_mutex_);
        const auto v = static_cast<long long>(std::llround(value));
        if (int r = iio_channel_attr_write_longlong(ch, attr, v); r < 0) {
            fail(std::string("cannot set ") + what + " to " + std::to_string(v), r);
        }
    }

    void destroy_tx_locked() {
        if (tx_buf_) {
            iio_buffer_destroy(tx_buf_);
            tx_buf_ = nullptr;
        }
    }

    void clear_status(iio_device* dev) {
        std::uint32_t v = 0;
        if (iio_device_reg_read(dev, kStatusRegister, &v) == 0) (void)iio_device_reg_write(dev, kStatusRegister, v);
    }

    // Reads and clears the sticky status bits; disables itself when the
    // register is not reachable (e.g. no debugfs access).
    void check_status(iio_device* dev, std::uint32_t bit, std::atomic<bool>& flag, bool& supported) {
        if (!supported) return;
        std::uint32_t v = 0;
        if (iio_device_reg_read(dev, kStatusRegister, &v) != 0) {
            supported = false;
            return;
        }
        if (v & bit) {
            flag = true;
            (void)iio_device_reg_write(dev, kStatusRegister, v);
        }
    }

    std::string uri_;
    iio_context* ctx_ = nullptr;
    iio_device* phy_ = nullptr;
    iio_device* rx_dev_ = nullptr;
    iio_device* tx_dev_ = nullptr;
    iio_channel *rx_lo_ = nullptr, *tx_lo_ = nullptr, *rx_ = nullptr, *tx_ = nullptr, *temp_ = nullptr;
    iio_channel *rx_i_ = nullptr, *rx_q_ = nullptr, *tx_i_ = nullptr, *tx_q_ = nullptr;
    iio_channel *rx2_ = nullptr, *rx2_i_ = nullptr, *rx2_q_ = nullptr;
    iio_channel *tx2_ = nullptr, *tx2_i_ = nullptr, *tx2_q_ = nullptr;
    iio_channel* tx_ch_i_ = nullptr;
    std::string gain_mode_[2] = {"slow_attack", "slow_attack"};
    unsigned rx_mask_ = kRx1;

    std::mutex ctrl_mutex_;
    std::mutex rx_mutex_;
    std::mutex tx_mutex_;
    iio_buffer* rx_buf_ = nullptr;
    iio_buffer* tx_buf_ = nullptr;
    std::size_t tx_samples_ = 0;
    unsigned rx_bits_ = 12, rx_shift_ = 0, tx_bits_ = 12, tx_shift_ = 4;
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> rx_overflow_{false};
    std::atomic<bool> tx_underflow_{false};
    bool rx_status_ok_ = true;
    bool tx_status_ok_ = true;
};

}  // namespace

std::unique_ptr<Backend> make_iio_backend(const std::string& uri, unsigned timeout_ms) {
    return std::make_unique<IioBackend>(uri, timeout_ms);
}

bool iio_supported() { return true; }

}  // namespace radio
