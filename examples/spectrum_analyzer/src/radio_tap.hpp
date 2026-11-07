#pragma once
// RX2 of an AD9361/AD9363 shared with radio-controller.
//
// radio-controller owns the transceiver: RX1/TX1, the RX LO and the sample
// rate (both RX channels share them and arrive in one DMA buffer). It
// publishes contiguous RX2 blocks on its [zmq].rx2 socket; this backend reads
// them and sets the RX2 gain through radio-controller's control socket.
// The analyzer cannot retune: its display window stays inside the band
// radio-controller is tuned to (see FixedTuning).

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
    std::string tap = "tcp://127.0.0.1:5558";      ///< radio-controller [zmq].rx2
    std::string control = "tcp://127.0.0.1:5557";  ///< radio-controller [zmq].control
    std::chrono::milliseconds timeout{3000};       ///< no block for this long = device error
    bool enable_rx2 = true;                        ///< turn rx2_enabled on in radio-controller
};

/// Throws radio::DeviceError when radio-controller cannot be reached or has no RX2.
std::unique_ptr<radio::Backend> make_radio_tap_backend(const RadioTapOptions& options);

}  // namespace sa
