#pragma once
// Protocol decoders: UART (async serial), I2C and SPI, on the changes of a
// capture. They walk the edges, not samples, so a capture of seconds decodes
// in milliseconds.
//
// An analyzer is a decoder with its settings (which channels, baud rate,
// mode, ...); there can be several of a type (two UARTs, TX and RX).

#include "capture.hpp"

#include <forge/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace la {

enum class FrameKind : std::uint8_t { data = 0, start = 1, stop = 2, address = 3, error = 4 };

enum FrameFlag : std::uint16_t {
    kRead = 1,           ///< I2C address: read
    kNack = 2,           ///< I2C: not acknowledged
    kParityError = 4,    ///< UART
    kFramingError = 8,   ///< UART: stop bit low
    kRepeated = 16,      ///< I2C: repeated start
    kNoMosi = 32,        ///< SPI: MOSI not decoded
    kNoMiso = 64,        ///< SPI: MISO not decoded
    kBreak = 128,        ///< UART: line held low for a whole frame
    kIncomplete = 256,   ///< a word cut short (CS released, STOP in a byte, end of capture)
};

struct DecodedFrame {
    std::uint64_t start = 0, end = 0;  ///< ticks
    FrameKind kind = FrameKind::data;
    std::uint16_t flags = 0;
    std::uint32_t value = 0;   ///< data byte / word; I2C address: 7-bit address; SPI: MOSI
    std::uint32_t value2 = 0;  ///< SPI: MISO; I2C address: the raw byte (address << 1 | R/W)
    std::uint8_t bits = 8;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual std::vector<DecodedFrame> decode(const Capture& c) const = 0;
    /// The channel the frames are drawn on (UART: its line, I2C: SDA, SPI: MOSI or MISO).
    virtual unsigned display_channel() const = 0;
};

/// The types and their settings (for the UI): [{type, label, fields: [{key, label, kind, ...}]}].
const forge::json& decoder_types();
/// `settings` with the defaults filled in; throws std::invalid_argument on a bad value.
forge::json normalize_settings(const std::string& type, const forge::json& settings);
/// A decoder for normalized settings.
std::unique_ptr<Decoder> make_decoder(const std::string& type, const forge::json& settings);

/// "0x48", "72", "0b01001000" or "'H'".
std::string format_value(std::uint32_t value, unsigned bits, const std::string& radix);
/// One line for the data table and the exports, e.g. "Write to 0x48 ACK", "0x41 'A'".
std::string describe(const std::string& type, const DecodedFrame& f, const std::string& radix);
/// Short kind name: "data", "start", "stop", "address", "error".
const char* kind_name(FrameKind k);

}  // namespace la
