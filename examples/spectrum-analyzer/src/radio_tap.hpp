#pragma once
// An RX channel shared by radio-controller ([radio].tap_rx).
//
// radio-controller owns the transceiver: the RX LO and the sample rate (both
// RX channels of an AD9361 share them and arrive in one DMA buffer). It
// publishes blocks of the channels it shares as streams on its [forge].data
// socket (topics "rx1", "rx2"; forge.v1 Header + radio.v1.IqBlock + cs16
// samples); this backend reads one of them and, if allowed, sets that
// channel's gain through radio-controller's control endpoint (forge.v1). The analyzer
// cannot retune: its display window stays inside the band radio-controller is
// tuned to (see FixedTuning).

#include "backend.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace sa {

/// Tuning a non-tunable source delivers (LO, sample rate, block size).
struct Tuning {
    double lo_hz = 0;
    double rate_hz = 0;
    std::size_t block_samples = 0;
    bool operator==(const Tuning& o) const {
        return lo_hz == o.lo_hz && rate_hz == o.rate_hz && block_samples == o.block_samples;
    }
    bool operator!=(const Tuning& o) const { return !(*this == o); }
};

/// Implemented by backends whose LO and rate are set by someone else.
class FixedTuning {
public:
    virtual ~FixedTuning() = default;
    /// Tuning of the newest data; waits for the first block. Throws radio::DeviceError on timeout.
    virtual Tuning tuning() = 0;
    /// Tuning of the samples the last read_rx() returned.
    virtual Tuning last_read() const = 0;
};

struct RadioTapOptions {
    std::string data = "tcp://127.0.0.1:5555";     ///< radio-controller [forge].data
    std::string control = "tcp://127.0.0.1:5557";  ///< radio-controller [forge].control
    std::chrono::milliseconds timeout{3000};       ///< no block for this long = device error
    unsigned channel = 2;                          ///< RX channel to listen to (1 or 2)
    bool control_gain = true;                      ///< set that channel's gain (else leave it alone)
};

/// Throws radio::DeviceError when radio-controller cannot be reached or does not share the channel.
std::unique_ptr<radio::Backend> make_radio_tap_backend(const RadioTapOptions& options);

}  // namespace sa
