#pragma once
// Where the words come from: the FPGA through UIO (uio_source.cpp), or the
// simulator (sim.cpp). Both give the same words (wire.hpp).

#include <forge/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace la {

struct SourceConfig {
    double sample_hz = 100e6;          ///< sample rate (the FPGA divides its 100 MHz clock)
    std::uint32_t glitch_ticks = 0;    ///< ignore pulses shorter than this (0: off)
    std::uint32_t heartbeat_ticks = 0; ///< a time word at least this often (0: 1 ms)
    double threshold_v = 1.65;         ///< comparator threshold of the shield
};

struct SourceStats {
    std::uint64_t words = 0;      ///< read so far
    std::uint64_t lost = 0;       ///< words the FPGA could not write (buffer full)
    std::uint64_t overflows = 0;  ///< times the buffer was full
};

class Source {
public:
    virtual ~Source() = default;
    /// "sim:mixed", "uio:/dev/uio0", ...
    virtual std::string name() const = 0;
    /// The clock the sample rate is divided from.
    virtual double base_clock_hz() const { return 100e6; }
    /// Starts a capture: the first word read is a start word. Throws on failure.
    virtual void start(const SourceConfig& config) = 0;
    /// Stops it; read() then returns nothing more.
    virtual void stop() = 0;
    /// Appends the words that arrived (waits up to `timeout` for some); returns how many.
    virtual std::size_t read(std::vector<std::uint32_t>& out, std::chrono::milliseconds timeout) = 0;
    virtual SourceStats stats() const = 0;
    /// Shown on the panel (FPGA version, buffer size, ...).
    virtual forge::json info() const { return forge::json::object(); }
    /// Channel names that fit the source (the simulator's signals).
    virtual std::vector<std::string> channel_names() const { return {}; }
    /// Analyzers that fit the source ([{type, name, settings}]).
    virtual forge::json suggested_analyzers() const { return forge::json::array(); }
};

/// Sample rates the FPGA can make from 100 MHz (whole divisors, whole nanoseconds).
const std::vector<double>& sample_rates();

}  // namespace la
