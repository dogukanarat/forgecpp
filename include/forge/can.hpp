#pragma once
// CAN through SocketCAN (Linux): raw frames, and ISO-TP channels that carry
// c2 commands. Both are in the kernel (CAN_RAW, and CAN_ISOTP since 5.10): no
// library to depend on.
//
//   forge::can::Socket bus("can0", {.filters = {{0x120, 0x7F0}}});   // ids 0x120..0x12F
//   bus.send(forge::can::Frame::make(0x100, {0x01, 0x02}));
//   forge::can::Frame f;
//   if (bus.receive(f, 100ms).ok()) { ... f.id, f.bytes() ... }      // in a worker()
//
// Commands: [can].interface (with tx_id and rx_id) answers the application's
// commands on an ISO-TP channel, as [serial].device does on a tty: the same
// c2 frames and the same zephyrus framing (COBS + CRC), one framed piece per
// ISO-TP message. can::isotp_link() gives the options for a serial::Link of
// one's own (to call a device on the bus).
//
// A PC without a CAN adapter: `modprobe vcan; ip link add vcan0 type vcan;
// ip link set vcan0 up` (and `isotpsend`/`candump` from can-utils).

#include "forge/serial.hpp"
#include "forge/status.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace forge::can {

inline constexpr std::uint32_t standard_mask = 0x7FF;       ///< 11-bit ids
inline constexpr std::uint32_t extended_mask = 0x1FFFFFFF;  ///< 29-bit ids

struct Frame {
    std::uint32_t id = 0;
    bool extended = false;  ///< a 29-bit id
    bool remote = false;    ///< a remote request (classic CAN, no data)
    bool fd = false;        ///< a CAN FD frame (up to 64 bytes)
    std::uint8_t size = 0;
    std::array<std::uint8_t, 64> data{};

    std::span<const std::uint8_t> bytes() const { return std::span(data).first(size); }
    /// A classic frame (at most 8 bytes; more: std::invalid_argument).
    static Frame make(std::uint32_t id, std::initializer_list<std::uint8_t> bytes, bool extended = false);
    static Frame make(std::uint32_t id, std::span<const std::uint8_t> bytes, bool extended = false);
};

/// Frames whose id & mask == id & mask pass (the kernel filters them).
struct Filter {
    std::uint32_t id = 0;
    std::uint32_t mask = standard_mask;
    bool extended = false;
};

struct SocketOptions {
    std::vector<Filter> filters;  ///< empty: every frame
    bool fd = false;              ///< also send and receive CAN FD frames
};

/// A raw CAN socket on one interface. Thread safe for one sender and one
/// receiver at a time.
class Socket {
public:
    Socket(const std::string& interface, SocketOptions options = {});
    /// A descriptor that carries struct can_frame / canfd_frame datagrams
    /// (tests: one end of a socketpair). The socket closes it.
    explicit Socket(int fd, bool fd_frames = false);
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// EINVAL: a size the frame type cannot carry; ETIMEDOUT: the interface's
    /// queue stayed full for `timeout` (no one acknowledges on the bus).
    Status send(const Frame& frame, std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    /// The next frame, or ETIMEDOUT after `timeout`.
    Status receive(Frame& frame, std::chrono::milliseconds timeout);
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    bool fd_frames_ = false;
};

struct IsotpOptions {
    std::string interface;          ///< "can0"
    std::uint32_t tx_id = 0x701;    ///< the id this end sends with
    std::uint32_t rx_id = 0x700;    ///< the id this end listens to (the peer's tx_id)
    bool extended = false;          ///< 29-bit ids
    bool fd = false;                ///< CAN FD frames (64 bytes) under ISO-TP
    int padding = 0xCC;             ///< pad short frames with this byte (-1: no padding)
};

/// An ISO-TP socket: each write sends one message (up to 4095 bytes), each
/// read returns one. Throws std::runtime_error with the reason when the
/// interface is missing or the kernel has no CAN_ISOTP.
int open_isotp(const IsotpOptions& options);

/// serial::LinkOptions for c2 over ISO-TP: `base` with a fresh ISO-TP socket,
/// one framed piece per message, and a label such as "can0 0x701/0x700".
serial::LinkOptions isotp_link(const IsotpOptions& options, serial::LinkOptions base = {});

/// "can0 0x701/0x700" (tx/rx), for logs and the endpoint list.
std::string describe(const IsotpOptions& options);

}  // namespace forge::can
