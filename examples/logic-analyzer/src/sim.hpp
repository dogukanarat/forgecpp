#pragma once
// The simulator: four inputs with real traffic on them, as words in the
// FPGA's format, at the pace of the wall clock (or as fast as asked, for tests).
//
//   mixed   D0 UART TX 115200 8N1 (text), D1 I2C SCL, D2 I2C SDA (a sensor
//           at 0x48 read every 10 ms, 100 kHz), D3 PWM 1 kHz with a sweeping duty
//   spi     D0 CS, D1 SCK, D2 MOSI, D3 MISO: a flash read at 1 MHz (mode 0)
//   uart    D0 TX and D1 RX of a command/answer exchange at 9600 baud,
//           D2 a 10 kHz clock, D3 a 50 Hz heartbeat pulse
//
// Each signal is a train of transactions; a transaction's edges are worked
// out from its number alone, so any time window can be generated on its own.

#include "source.hpp"
#include "wire.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace la {

struct ChannelEdge {
    std::uint64_t tick;
    std::uint8_t channel;
    bool level;
};

/// One signal source of a scenario: transactions every `period_s`, starting at `offset_s`.
class Generator {
public:
    virtual ~Generator() = default;
    virtual double period_s() const = 0;
    virtual double offset_s() const { return 0; }
    /// Upper bound of a transaction's length (<= period).
    virtual double length_s() const = 0;
    /// Edges of transaction `k`, in seconds from its start.
    virtual void transaction(std::uint64_t k, std::vector<std::pair<double, std::pair<unsigned, bool>>>& out) const = 0;
    /// Idle levels of its channels, as bits.
    virtual std::uint8_t idle_levels() const = 0;
    virtual std::uint8_t channel_mask() const = 0;
};

struct Scenario {
    std::string name;
    std::array<std::string, kChannels> names;
    std::vector<std::shared_ptr<Generator>> generators;
    forge::json analyzers = forge::json::array();  ///< what to decode it with

    std::uint8_t initial() const;
    /// Changes in (from, to] at `tick_hz`, sorted.
    void edges(std::uint64_t from, std::uint64_t to, double tick_hz, std::vector<ChannelEdge>& out) const;
};

/// "mixed", "spi" or "uart"; throws std::invalid_argument for another name.
Scenario make_scenario(const std::string& name);
const std::vector<std::string>& scenario_names();

/// Bytes of an UART frame as edges (helper for tests): idle high, LSB first.
void uart_edges(double t0, double baud, const std::vector<std::uint8_t>& bytes, unsigned ch,
                std::vector<std::pair<double, std::pair<unsigned, bool>>>& out, unsigned bits = 8,
                const std::string& parity = "none", double stop = 1);

class SimSource : public Source {
public:
    /// realtime = false: each read() moves time on by `step` instead of waiting for the clock.
    SimSource(Scenario scenario, bool realtime = true, std::chrono::milliseconds step = std::chrono::milliseconds(20));

    std::string name() const override { return "sim:" + scenario_.name; }
    void start(const SourceConfig& config) override;
    void stop() override;
    std::size_t read(std::vector<std::uint32_t>& out, std::chrono::milliseconds timeout) override;
    SourceStats stats() const override { return stats_; }
    forge::json info() const override;
    std::vector<std::string> channel_names() const override;
    forge::json suggested_analyzers() const override { return scenario_.analyzers; }

private:
    void generate(std::uint64_t to, std::vector<std::uint32_t>& out);

    Scenario scenario_;
    bool realtime_;
    std::chrono::milliseconds step_;
    bool running_ = false;
    double tick_hz_ = 100e6;
    std::uint64_t heartbeat_ = 100000;
    std::uint64_t done_ = 0;           ///< tick of the last word written
    std::uint64_t pending_until_ = 0;  ///< edges generated up to this tick
    std::uint8_t levels_ = 0;
    std::vector<std::uint32_t> words_;
    std::vector<ChannelEdge> edges_;
    std::chrono::steady_clock::time_point t0_;
    SourceStats stats_;
};

}  // namespace la
