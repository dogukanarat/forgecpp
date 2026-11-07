#pragma once
// Hardware abstraction for AD936x transceivers on ADI's ad9361 driver:
// AD9364 (1 RX + 1 TX) and AD9361/AD9363 (2 RX + 2 TX).
//
// Both RX channels of an AD9361 share the RX LO, the sample rate and the RF
// bandwidth, and arrive in one DMA buffer; both TX channels share the TX LO.
// Only gain (RX) and attenuation (TX) are per channel. So one process owns
// the transceiver: start_rx() takes a mask of the RX channels to stream, the
// gain / attenuation calls and start_tx() take a channel index (0 = RX1/TX1,
// 1 = RX2/TX2).
//
// Samples cross this interface as "cs16": interleaved I/Q int16 pairs at full
// scale (+-32767), in both directions. Backends convert to and from the
// hardware format (the AD936x has 12-bit converters).
//
// Threading: control calls (set_*, read-backs) come from the application main
// loop; RX streaming runs on the RX thread and TX streaming on the TX thread.
// cancel_rx() may be called from any thread to unblock read_rx().

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace radio {

/// Errors reported by the device or the transport (lost connection, rejected value).
class DeviceError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// AD9364 limits (datasheet / ad9361 driver, without custom FIR filters).
struct Limits {
    static constexpr double lo_min_hz = 70e6;
    static constexpr double lo_max_hz = 6000e6;
    static constexpr double rate_min_hz = 2.083334e6;
    static constexpr double rate_max_hz = 61.44e6;
    static constexpr double bw_min_hz = 0.2e6;
    static constexpr double bw_max_hz = 56e6;
    static constexpr double rx_gain_min_db = -3;
    static constexpr double rx_gain_max_db = 71;
    static constexpr double tx_atten_min_db = 0;
    static constexpr double tx_atten_max_db = 89.75;
};

inline const std::vector<std::string>& gain_modes() {
    static const std::vector<std::string> modes{"manual", "slow_attack", "fast_attack", "hybrid"};
    return modes;
}

/// Complete RF configuration, applied with Backend::apply().
struct Settings {
    double rx_lo_hz = 2400e6;
    double tx_lo_hz = 2400e6;
    double sample_rate_hz = 30.72e6;
    double rx_bandwidth_hz = 18e6;
    double tx_bandwidth_hz = 18e6;
    std::string gain_mode = "slow_attack";
    double rx_gain_db = 30;          ///< used in manual gain mode
    double tx_attenuation_db = 10;   ///< 0 .. 89.75 dB, 0.25 dB steps
    // RX2 (AD9361/AD9363 only; ignored by apply() on single-RX parts)
    std::string rx2_gain_mode = "slow_attack";
    double rx2_gain_db = 30;
    unsigned tx_channel = 0;         ///< TX channel tx_attenuation_db applies to (not persisted)
};

nlohmann::json to_json(const Settings& s);
Settings settings_from_json(const nlohmann::json& j, Settings base = {});

class Backend {
public:
    virtual ~Backend() = default;

    /// "sim" or "iio:<uri>".
    virtual std::string name() const = 0;
    /// Device description for the UI (model, driver, firmware, serial, ...).
    virtual nlohmann::json describe() const = 0;

    // --- control (main loop) ---------------------------------------------
    virtual void set_rx_lo(double hz) = 0;
    virtual void set_tx_lo(double hz) = 0;
    virtual void set_sample_rate(double hz) = 0;
    virtual void set_rx_bandwidth(double hz) = 0;
    virtual void set_tx_bandwidth(double hz) = 0;
    /// Gain control of RX channel `channel` (0 = RX1, 1 = RX2).
    virtual void set_gain_mode(const std::string& mode, unsigned channel = 0) = 0;
    virtual void set_rx_gain(double db, unsigned channel = 0) = 0;  ///< manual gain mode only
    virtual void set_tx_attenuation(double db, unsigned channel = 0) = 0;
    /// Current RX gain (changes on its own under AGC).
    virtual double rx_gain(unsigned channel = 0) = 0;
    virtual std::optional<double> temperature_c() = 0;
    /// Number of RX / TX channels: 1 (AD9364) or 2 (AD9361/AD9363 in 2R2T mode).
    virtual unsigned rx_channel_count() const = 0;
    virtual unsigned tx_channel_count() const = 0;

    /// Apply every setting in a sensible order.
    void apply(const Settings& s);

    // --- RX streaming (RX thread) ------------------------------------------
    /// `channel_mask`: bit 0 = RX1, bit 1 = RX2 (both: same buffer, same sample
    /// clock). Throws DeviceError when the transceiver lacks a channel.
    virtual void start_rx(std::size_t samples_per_buffer, unsigned channel_mask) = 0;
    void start_rx(std::size_t samples_per_buffer) { start_rx(samples_per_buffer, kRx1); }
    static constexpr unsigned kRx1 = 1, kRx2 = 2;
    /// Block until a buffer is available and copy up to `max_samples` complex
    /// samples (cs16) of RX1 into `rx1` and of RX2 into `rx2`; a channel that
    /// is not streamed or a null pointer is skipped. Returns the number of
    /// samples per channel, 0 when cancelled. Throws DeviceError.
    virtual std::size_t read_rx(std::int16_t* rx1, std::int16_t* rx2, std::size_t max_samples) = 0;
    std::size_t read_rx(std::int16_t* iq, std::size_t max_samples) { return read_rx(iq, nullptr, max_samples); }
    virtual void stop_rx() = 0;
    /// Thread-safe: make a pending read_rx() return.
    virtual void cancel_rx() = 0;
    /// True once after the hardware reported an RX overflow (lost samples).
    virtual bool take_rx_overflow() = 0;

    // --- TX streaming (TX thread) ------------------------------------------
    /// `cyclic`: the next write is repeated by the hardware until stop_tx().
    /// `channel`: 0 = TX1, 1 = TX2.
    virtual void start_tx(std::size_t samples_per_buffer, bool cyclic, unsigned channel) = 0;
    void start_tx(std::size_t samples_per_buffer, bool cyclic) { start_tx(samples_per_buffer, cyclic, 0); }
    /// Write exactly `samples_per_buffer` complex samples (cs16). Blocks while the queue is full.
    virtual void write_tx(const std::int16_t* iq, std::size_t samples) = 0;
    virtual void stop_tx() = 0;
    /// True once after the hardware reported a TX underflow (gap in the output).
    virtual bool take_tx_underflow() = 0;
};

struct SimOptions {
    bool loopback = true;      ///< couple a TX channel into an RX channel (a cable) when the LOs are close
    unsigned loopback_tx = 0;  ///< 0 = TX1, 1 = TX2
    unsigned loopback_rx = 0;  ///< 0 = RX1, 1 = RX2   (e.g. 0 -> 1: a TX1 -> RX2 cable)
    bool loopback_split = false;  ///< TX into every RX channel (a power splitter), not only loopback_rx
    double loopback_db = -20;     ///< TX output -> RX input (cable, splitter, attenuators)
    double loopback_cfo_hz = 0;   ///< frequency offset of the received copy (an oscillator error)
    unsigned channels = 1;     ///< 1: AD9364, 2: AD9361 (RX1/RX2 see the same air, independent noise)
    unsigned seed = 1;
};

std::unique_ptr<Backend> make_sim_backend(SimOptions options = {});

/// `uri`: "local:", "ip:192.168.2.1", "usb:1.2.5", ... ("" = libiio default context).
/// Throws DeviceError if the device cannot be opened or libiio support is not built in.
std::unique_ptr<Backend> make_iio_backend(const std::string& uri, unsigned timeout_ms);
bool iio_supported();

// --- sample format conversion (also used by the tests) ----------------------
// libiio describes a channel with `bits` significant bits stored `shift` bits
// up in a 16-bit word (AD936x: RX le:S12/16>>0, TX MSB aligned).

/// Hardware word -> full-scale cs16 component.
inline std::int16_t to_full_scale(std::int16_t word, unsigned bits, unsigned shift) {
    const unsigned up = 16u - bits - shift;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(word) << up);
}

/// Full-scale cs16 component -> hardware word (sign-extended, placed at `shift`).
inline std::int16_t from_full_scale(std::int16_t value, unsigned bits, unsigned shift) {
    const int significant = value >> (16 - static_cast<int>(bits));  // arithmetic shift keeps the sign
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(significant) << shift);
}

}  // namespace radio
