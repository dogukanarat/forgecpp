#pragma once
// Hardware abstraction for AD936x transceivers (AD9364: 1 RX + 1 TX).
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
    virtual void set_gain_mode(const std::string& mode) = 0;
    virtual void set_rx_gain(double db) = 0;          ///< manual gain mode only
    virtual void set_tx_attenuation(double db) = 0;
    /// Current RX gain (changes on its own under AGC).
    virtual double rx_gain() = 0;
    virtual std::optional<double> temperature_c() = 0;

    /// Apply every setting in a sensible order.
    void apply(const Settings& s);

    // --- RX streaming (RX thread) ------------------------------------------
    virtual void start_rx(std::size_t samples_per_buffer) = 0;
    /// Block until a buffer is available and copy up to `max_samples` complex
    /// samples (cs16) into `iq`. Returns 0 when cancelled. Throws DeviceError.
    virtual std::size_t read_rx(std::int16_t* iq, std::size_t max_samples) = 0;
    virtual void stop_rx() = 0;
    /// Thread-safe: make a pending read_rx() return.
    virtual void cancel_rx() = 0;
    /// True once after the hardware reported an RX overflow (lost samples).
    virtual bool take_rx_overflow() = 0;

    // --- TX streaming (TX thread) ------------------------------------------
    /// `cyclic`: the next write is repeated by the hardware until stop_tx().
    virtual void start_tx(std::size_t samples_per_buffer, bool cyclic) = 0;
    /// Write exactly `samples_per_buffer` complex samples (cs16). Blocks while the queue is full.
    virtual void write_tx(const std::int16_t* iq, std::size_t samples) = 0;
    virtual void stop_tx() = 0;
    /// True once after the hardware reported a TX underflow (gap in the output).
    virtual bool take_tx_underflow() = 0;
};

struct SimOptions {
    bool loopback = true;   ///< mix transmitted samples into RX when the LOs are close
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
