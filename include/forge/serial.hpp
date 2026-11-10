#pragma once
// c2 frames on a serial line (UART, USB CDC), framed as zephyrus frames them
// (modules/lib/cobs): a zephyrus device on a UART is a peer of the program.
//
// On the wire a c2 frame is one message. A message is cut into fragments that
// fit one frame of `frame_size` bytes; each frame is
//
//   0x00 | COBS( flags | seq | total | part of the message | CRC16 ) | 0x00
//
// flags: bit 0 first fragment, bit 1 last fragment; seq counts from 0; total
// is the number of fragments (at most 255). The CRC is CRC16-CCITT (poly
// 0x1021, start 0xFFFF) of the bytes before it, little-endian. COBS removes
// every zero byte, so 0x00 only marks a boundary: a receiver that starts in
// the middle, or loses bytes, finds the next frame by itself. Nothing is
// acknowledged or sent again; c2 answers (or their absence) tell the sender.
//
//   forge::serial::Link link({.device = "/dev/ttyUSB0", .baud = 115200});
//   my_reply r{};
//   auto s = link.call(my_msg_id_status, my_request{}, r);   // ask the device
//
// In an application, [forge].serial = "/dev/ttyUSB0" answers the program's
// commands on that line, like [forge].control does on ZeroMQ.

#include "forge/c2.hpp"
#include "forge/pb.hpp"
#include "forge/status.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace forge::serial {

/// CRC16-CCITT (poly 0x1021, start 0xFFFF, no reflection, no final XOR).
std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t crc = 0xFFFF);

/// One frame: delimiter, COBS of payload + CRC, delimiter. Appended to `out`.
void encode_frame(std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out);

/// Undoes encode_frame for the bytes between two delimiters (without them):
/// the payload without its CRC. EBADMSG: a zero byte; EOVERFLOW: a block
/// longer than the frame; EMSGSIZE: shorter than a CRC; EILSEQ: the CRC does
/// not match.
Status decode_frame(std::span<const std::uint8_t> encoded, std::vector<std::uint8_t>& payload);

struct FramingOptions {
    /// Largest frame sent, both delimiters included. Must not be larger than
    /// the peer's receive buffer (zephyrus: CONFIG_ZEPHYRUS_COBS_FRAME_SIZE,
    /// 244 by default). Frames received may be larger, up to one whole message.
    std::size_t frame_size = 244;
    /// Largest message, both ways (zephyrus: CONFIG_ZEPHYRUS_COBS_MAX_MESSAGE_SIZE,
    /// 4096 by default). forgecpp's longest replies (controls, commands) need
    /// about 12.5 kB; a peer that asks for them needs a buffer that large.
    std::size_t max_message = 16384;
};

/// Messages to frames and frames to messages, as zephyrus's cobs_framing.
/// Not thread safe: one sender and one receiver, or a lock around it.
class Framing {
public:
    struct Stats {
        std::uint64_t messages_sent = 0, fragments_sent = 0;
        std::uint64_t messages_received = 0, fragments_received = 0;
        std::uint64_t frames_dropped = 0;      ///< bad COBS or CRC
        std::uint64_t messages_dropped = 0;    ///< a fragment out of order, or too long
        std::uint64_t fragments_orphaned = 0;  ///< the rest of a dropped message
    };

    explicit Framing(FramingOptions options = {});

    /// Largest part of a message in one frame.
    std::size_t fragment_payload() const;

    /// The frames of one message, one after the other, in `out`. EINVAL for an
    /// empty message, EMSGSIZE for one too long.
    Status encode(std::span<const std::uint8_t> message, std::vector<std::uint8_t>& out);

    /// Bytes from the line: `deliver` gets every complete message.
    void receive(std::span<const std::uint8_t> bytes,
                 const std::function<void(std::span<const std::uint8_t>)>& deliver);

    Stats stats() const { return stats_; }

private:
    void frame_done(const std::function<void(std::span<const std::uint8_t>)>& deliver);

    FramingOptions options_;
    std::vector<std::uint8_t> rx_;  // the encoded frame being read
    bool in_frame_ = false;
    std::vector<std::uint8_t> decoded_;
    std::vector<std::uint8_t> message_;  // the message being assembled
    bool assembling_ = false;
    std::uint8_t expected_seq_ = 0, total_ = 0;
    Stats stats_;
};

/// A tty in raw mode, 8N1, no flow control (or any file descriptor).
class Port {
public:
    /// Opens `device` at `baud` (a standard rate: 9600 ... 4000000).
    Port(const std::string& device, int baud);
    /// Takes `fd` as it is (a pty, a socket); closes it at the end.
    explicit Port(int fd);
    ~Port();
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    int fd() const { return fd_; }
    /// Writes all of `bytes`.
    /// Write everything, waiting at most `timeout` for a line that takes no
    /// bytes (ETIMEDOUT). The port is non-blocking.
    Status write(std::span<const std::uint8_t> bytes, std::chrono::milliseconds timeout = std::chrono::seconds(1));
    /// What is there, waiting at most `timeout`: 0 bytes after the timeout.
    /// EIO when the line is gone (unplugged).
    Status read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout, std::size_t& got);

private:
    int fd_ = -1;
};

struct LinkOptions {
    std::string device;  ///< empty: `fd` is used
    int baud = 115200;
    int fd = -1;  ///< an open descriptor (tests: one end of a pty); the link closes it
    FramingOptions framing;
    /// How long call() waits for the first answer.
    std::chrono::milliseconds timeout{1000};
    /// Requests from the other end. Each runs on the link's worker thread and
    /// answers through `send` (tmtc::Dispatcher::handle fits). Without it, a
    /// request gets a NACK ENOSYS.
    std::function<void(std::span<const std::uint8_t> frame,
                       const std::function<void(std::span<const std::uint8_t>)>& send)>
        on_request;
    /// Requests waiting for the worker; one more gets a NACK EAGAIN.
    std::size_t queue = 8;
    /// Reports (push frames) from the other end, on the reader thread.
    std::function<void(const c2::Message&)> on_push;
    std::string thread_name = "forge-serial";
    /// What device() says when `fd` is used ("can0 0x701/0x700"); empty: "fd:<n>".
    std::string label;
    /// Write each frame on its own: a transport where one write is one
    /// message (an ISO-TP socket, can::isotp_link()) instead of a byte stream.
    bool frame_per_write = false;
};

/// c2 on a serial line, both ways: requests from here (call), requests from
/// the other end (on_request) and reports (push, on_push). A reader thread
/// takes the bytes; a worker thread runs the requests that came in, so a
/// handler may itself call() the other end.
class Link {
public:
    struct Stats {
        Framing::Stats framing;
        std::uint64_t requests_in = 0, requests_refused = 0, answers_unmatched = 0;
    };

    explicit Link(LinkOptions options);
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    /// One request to the other end; each answer goes to `on_answer` (several
    /// for a reply in parts). Returns the status of a NACK, ETIMEDOUT without
    /// an answer, or what on_answer returned. Any thread; several at once.
    Status request(std::uint16_t id, std::span<const std::uint8_t> payload,
                   const std::function<Status(const c2::Message&)>& on_answer);

    /// A command with one reply (an ACK leaves `rsp` empty).
    template <pb::Message Req, pb::Message Rsp>
    Status call(std::uint16_t id, const Req& req, Rsp& rsp) {
        rsp = Rsp{};
        return call(id, *pb::type_of<Req>, &req,
                    [&](const c2::Message& m) { return m.payload.empty() ? Status() : pb::decode(m.payload, rsp); });
    }
    /// A command that answers in parts: `each` gets every part.
    template <pb::Message Req, pb::Message Rsp>
    Status call_parts(std::uint16_t id, const Req& req, const std::function<void(const Rsp&)>& each) {
        return call(id, *pb::type_of<Req>, &req, [&](const c2::Message& m) {
            Rsp rsp{};
            if (auto s = pb::decode(m.payload, rsp); !s.ok()) return s;
            each(rsp);
            return Status();
        });
    }

    /// A report to the other end (a push frame; nothing answers it).
    template <pb::Message T>
    Status push(std::uint16_t id, const T& body) {
        return push(id, *pb::type_of<T>, &body);
    }
    Status push(std::uint16_t id, const pb::Type& type, const void* body);

    /// One c2 frame as it is (already encoded).
    Status send(std::span<const std::uint8_t> frame);

    /// False once the line is gone (EIO: unplugged); the link does not reopen it.
    bool connected() const;
    Stats stats() const;
    const std::string& device() const { return device_; }

private:
    struct Pending {
        std::vector<std::vector<std::uint8_t>> frames;  // the answers, as received
        bool done = false;
    };

    Status call(std::uint16_t id, const pb::Type& type, const void* req,
                const std::function<Status(const c2::Message&)>& on_answer);
    void read_loop(const std::stop_token& stop);
    void work_loop(const std::stop_token& stop);
    void on_message(std::span<const std::uint8_t> message);
    void refuse(const c2::Header& request, const Status& why);

    LinkOptions options_;
    std::string device_;
    Port port_;

    mutable std::mutex tx_mutex_;  // one message at a time on the wire
    Framing tx_framing_;
    std::vector<std::uint8_t> tx_buffer_;

    Framing rx_framing_;  // reader thread only

    mutable std::mutex mutex_;
    std::condition_variable answered_;
    std::map<std::uint32_t, Pending> pending_;  // (cmd_id << 16 | seq)
    std::uint16_t seq_ = 0;
    bool connected_ = true;
    Stats stats_;  // stats_.framing: the reader's counters, copied after each read

    std::condition_variable_any work_ready_;
    std::deque<std::vector<std::uint8_t>> work_;

    std::jthread worker_;
    std::jthread reader_;
};

}  // namespace forge::serial
