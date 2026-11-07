// 16QAM burst modem: framing, modulation, detection, demodulation.
//
// Plain C++ without forgecpp, so it can be tested (and reused) on its own.
//
// Packet on the air (symbols at sample_rate / sps, RRC pulses):
//
//   | preamble 64 QPSK | header 24 QPSK          | payload + CRC32, scrambled, 16QAM | gap |
//   | 32 PN x 2        | len, seq, CRC16 (48 bit) | 2 symbols per byte                | 16 0|
//
// Receiver, per packet: Schmidl & Cox on the two preamble halves (detection and
// frequency offset) -> correlation with the preamble waveform (sample timing to
// a fraction of a sample) -> matched filter every half symbol -> decision
// feedback equalizer trained on the preamble by least squares (gain, phase,
// echoes) -> phase loop and gain loop, the equalizer taps adapting (all
// decision directed) -> header -> payload, CRC32.
// The sample clock is assumed to be steady within a packet (one transceiver, or
// clocks within a few tens of ppm).
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace qam {

using cf = std::complex<float>;

struct Params {
    double sample_rate = 3.0e6;       ///< Hz
    unsigned sps = 4;                 ///< samples per symbol
    double rolloff = 0.35;            ///< RRC excess bandwidth
    unsigned span = 8;                ///< RRC length, symbols
    double tx_rms = 0.25;             ///< RMS of the TX signal, full scale = 1 (-12 dBFS)
    double detect_threshold = 0.6;    ///< Schmidl & Cox metric (0..1)
    std::size_t max_payload = 1500;   ///< bytes
    /// Decision feedback equalizer, trained on the preamble (least squares) and
    /// followed through the packet (LMS): echoes (multipath). Off: gain and phase only.
    bool equalizer = true;
    unsigned eq_ff = 8;               ///< feed-forward taps, T/2 spaced (4 symbols)
    unsigned eq_lead = 3;             ///< ... of them ahead of the symbol (1.5 symbols)
    unsigned eq_fb = 3;               ///< feedback taps, symbols (echoes up to ~4 us at 750 ksym/s)
    double symbol_rate() const { return sample_rate / sps; }
    /// Occupied bandwidth of the signal, Hz.
    double occupied_bandwidth() const { return symbol_rate() * (1 + rolloff); }
};

constexpr unsigned kPreambleHalf = 32;
constexpr unsigned kPreambleSymbols = 2 * kPreambleHalf;
constexpr unsigned kHeaderSymbols = 24;
constexpr unsigned kGapSymbols = 16;

// --- building blocks (public for the tests) ------------------------------------
std::uint16_t crc16(const std::uint8_t* data, std::size_t n);  ///< CRC-16/CCITT-FALSE
std::uint32_t crc32(const std::uint8_t* data, std::size_t n);  ///< IEEE 802.3
/// Additive scrambler (x^7 + x^4 + 1, as 802.11): the same call descrambles.
void scramble(std::uint8_t* data, std::size_t n);
/// 64 QPSK symbols of unit magnitude: a 32-symbol PN sequence, twice.
const std::vector<cf>& preamble();
/// Gray-coded 16QAM, unit average energy: bits 3-2 -> I, 1-0 -> Q.
cf map16(unsigned nibble);
unsigned demap16(cf symbol);
/// RRC taps at the sample rate, sum of squares 1 (span * sps + 1 taps).
std::vector<float> rrc_taps(const Params& p);
/// Symbols of a whole packet (without the gap).
std::vector<cf> packet_symbols(const std::uint8_t* payload, std::size_t n, std::uint16_t seq);
/// Symbols on the air for a payload of `n` bytes, the gap included.
std::size_t packet_symbol_count(std::size_t n);

/// Turns packets into a continuous cs16 sample stream (zeros while idle).
class Modulator {
public:
    /// Next packet to send: fill `payload` and `seq`, return false when there is none.
    using Next = std::function<bool(std::vector<std::uint8_t>& payload, std::uint16_t& seq)>;

    explicit Modulator(const Params& p);
    /// Fill `n` interleaved int16 I/Q samples.
    void fill(std::int16_t* iq, std::size_t n, const Next& next);

    std::uint64_t packets() const { return packets_; }
    std::uint64_t clipped() const { return clipped_; }  ///< samples clipped at full scale
    bool idle() const { return pos_ >= symbols_.size() && gap_ == 0; }

private:
    void next_symbol_samples(cf a);

    Params p_;
    std::vector<float> taps_;
    std::vector<float> poly_;        // sps rows of span + 1 taps (scaled)
    unsigned len_ = 0;               // span + 1
    std::vector<float> hist_re_, hist_im_;  // last len_ symbols, twice (newest at head_)
    unsigned head_ = 0;
    std::size_t quiet_ = 0;          // zero symbols in a row
    std::vector<cf> symbols_;        // current packet
    std::size_t pos_ = 0;
    unsigned gap_ = 0;
    std::vector<std::int16_t> ready_;
    std::size_t ready_pos_ = 0;
    float scale_;
    std::vector<std::uint8_t> payload_;
    std::uint64_t packets_ = 0, clipped_ = 0;
};

struct Packet {
    std::vector<std::uint8_t> payload;
    std::uint16_t seq = 0;
    float evm_db = 0;     ///< RMS error vector of the payload symbols, dB below the average symbol
    float cfo_hz = 0;     ///< frequency offset measured on the preamble
    float level_db = 0;   ///< received symbol amplitude, dBFS
    double timing = 0;    ///< start of the packet, samples since the receiver started
};

struct Stats {
    std::uint64_t detected = 0;       ///< preambles found
    std::uint64_t header_errors = 0;  ///< header CRC16 failed (or a false detection)
    std::uint64_t crc_errors = 0;     ///< payload CRC32 failed
    std::uint64_t packets = 0;        ///< delivered
    std::uint64_t samples = 0;
    std::uint64_t resets = 0;         ///< input gaps
};

/// Finds and decodes packets in a stream of cs16 samples.
class Demodulator {
public:
    using OnPacket = std::function<void(Packet&&)>;

    explicit Demodulator(const Params& p);
    void push(const std::int16_t* iq, std::size_t n, const OnPacket& on_packet);
    /// Samples were lost: drop what is buffered and search again.
    void reset();

    Stats stats() const;
    /// The latest payload symbols after correction (for a constellation view). Thread-safe.
    std::vector<cf> constellation() const;

private:
    enum class State { search, peak, decode };
    std::int64_t end() const { return base_ + static_cast<std::int64_t>(x_.size()); }
    const cf& at(std::int64_t n) const { return x_[static_cast<std::size_t>(n - base_)]; }
    void restart_sums(std::int64_t n);
    bool try_decode(const OnPacket& on_packet);  // false: needs more samples
    void trim();

    Params p_;
    std::vector<float> taps_;
    std::vector<cf> wave_;           // preamble waveform (correlation template)
    double wave_energy_ = 0;
    unsigned L_;                     // samples per preamble half
    unsigned D_;                     // RRC group delay, samples

    std::vector<cf> x_;              // samples from base_ on
    std::int64_t base_ = 0;
    std::int64_t pos_ = 0;           // next detector position
    std::complex<double> P_{};
    double E1_ = 0, E2_ = 0;
    bool sums_ = false;

    State state_ = State::search;
    double best_metric_ = 0;
    std::int64_t best_n_ = 0;
    std::complex<double> best_P_{};

    Stats stats_;
    mutable std::mutex view_mutex_;
    std::vector<cf> constellation_;
    std::size_t constellation_pos_ = 0;
};

}  // namespace qam
