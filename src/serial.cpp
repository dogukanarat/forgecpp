// c2 frames on a serial line, framed byte for byte as zephyrus's
// modules/lib/cobs (cobs.c, cobs_framing.c): the same encoder, the same
// receiver states, the same decisions on a bad or missing fragment.
#include "forge/serial.hpp"

#include "forge/log.hpp"
#include "forge/trace.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace forge::serial {
namespace {

constexpr std::uint8_t delimiter = 0x00;
constexpr std::size_t crc_size = 2;
constexpr std::size_t header_size = 3;  // flags, seq, total
constexpr std::uint8_t flag_first = 1U << 0;
constexpr std::uint8_t flag_last = 1U << 1;
constexpr std::size_t max_fragments = 255;
constexpr std::uint8_t code_max = 0xFF;  // 254 bytes without a zero
constexpr std::size_t code_block = 254;
constexpr std::size_t encode_cost = 5;  // two delimiters, the first code byte, the CRC

Status error(int code, const char* what) { return Status::error(module_id::serial, code, what); }

// The largest encoded frame for `payload` bytes (the worst case: no zeros).
std::size_t frame_bytes(std::size_t payload) {
    const auto stuffed = payload + crc_size;
    return stuffed + stuffed / code_block + 3;
}

// COBS that continues across calls, so the header, the data and the CRC are
// one run (as cobs_encode_frame_parts does).
struct Encoder {
    std::vector<std::uint8_t>& out;
    std::size_t code_idx;
    std::uint8_t code = 1;

    explicit Encoder(std::vector<std::uint8_t>& o) : out(o) {
        out.push_back(delimiter);
        code_idx = out.size();
        out.push_back(0);  // the first code byte, written at the end of its block
    }
    void add(std::span<const std::uint8_t> bytes) {
        for (const auto b : bytes) {
            if (b == 0) {
                close_block();
                continue;
            }
            out.push_back(b);
            if (++code == code_max) close_block();
        }
    }
    void close_block() {
        out[code_idx] = code;
        code = 1;
        code_idx = out.size();
        out.push_back(0);
    }
    void finish() {
        out[code_idx] = code;
        out.push_back(delimiter);
    }
};

speed_t speed_of(int baud) {
    switch (baud) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 500000: return B500000;
        case 921600: return B921600;
        case 1000000: return B1000000;
        case 1500000: return B1500000;
        case 2000000: return B2000000;
        case 3000000: return B3000000;
        case 4000000: return B4000000;
        default: return B0;
    }
}

std::uint32_t key_of(std::uint16_t cmd_id, std::uint16_t seq) { return (std::uint32_t{cmd_id} << 16) | seq; }

}  // namespace

std::uint16_t crc16(std::span<const std::uint8_t> data, std::uint16_t crc) {
    for (const auto byte : data) {
        crc = static_cast<std::uint16_t>(crc ^ (std::uint16_t{byte} << 8));
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000U) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021U)
                                       : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

void encode_frame(std::span<const std::uint8_t> payload, std::vector<std::uint8_t>& out) {
    const auto crc = crc16(payload);
    const std::uint8_t tail[crc_size] = {static_cast<std::uint8_t>(crc & 0xFF), static_cast<std::uint8_t>(crc >> 8)};
    out.reserve(out.size() + frame_bytes(payload.size()));
    Encoder e(out);
    e.add(payload);
    e.add(tail);
    e.finish();
}

Status decode_frame(std::span<const std::uint8_t> encoded, std::vector<std::uint8_t>& payload) {
    payload.clear();
    std::size_t i = 0;
    while (i < encoded.size()) {
        const auto code = encoded[i++];
        if (code == 0) return error(EBADMSG, "a zero inside a frame");
        const std::size_t copy = code - 1U;
        if (i + copy > encoded.size()) return error(EOVERFLOW, "a COBS block longer than the frame");
        payload.insert(payload.end(), encoded.begin() + static_cast<std::ptrdiff_t>(i),
                       encoded.begin() + static_cast<std::ptrdiff_t>(i + copy));
        i += copy;
        if (code != code_max && i < encoded.size()) payload.push_back(0);
    }
    if (payload.size() < crc_size) return error(EMSGSIZE, "a frame without a checksum");
    const auto n = payload.size() - crc_size;
    const auto received = static_cast<std::uint16_t>(payload[n] | (payload[n + 1] << 8));
    payload.resize(n);
    if (received != crc16(payload)) return error(EILSEQ, "a frame with a wrong checksum");
    return {};
}

// --- Framing -------------------------------------------------------------------------

Framing::Framing(FramingOptions options) : options_(options) {
    if (options_.frame_size < 16) options_.frame_size = 16;  // zephyrus's Kconfig range
}

std::size_t Framing::fragment_payload() const {
    return options_.frame_size - header_size - encode_cost - options_.frame_size / code_block;
}

Status Framing::encode(std::span<const std::uint8_t> message, std::vector<std::uint8_t>& out) {
    if (message.empty()) return error(EINVAL, "an empty message");
    const auto part = fragment_payload();
    const auto fragments = (message.size() + part - 1) / part;
    if (message.size() > options_.max_message || fragments > max_fragments) {
        return error(EMSGSIZE, "a message too long for the link");
    }
    for (std::size_t seq = 0, offset = 0; seq < fragments; ++seq) {
        const auto n = std::min(part, message.size() - offset);
        std::uint8_t flags = 0;
        if (seq == 0) flags |= flag_first;
        if (seq == fragments - 1) flags |= flag_last;
        const std::uint8_t header[header_size] = {flags, static_cast<std::uint8_t>(seq),
                                                  static_cast<std::uint8_t>(fragments)};
        const auto chunk = message.subspan(offset, n);
        const auto crc = crc16(chunk, crc16(header));
        const std::uint8_t tail[crc_size] = {static_cast<std::uint8_t>(crc & 0xFF),
                                             static_cast<std::uint8_t>(crc >> 8)};
        Encoder e(out);
        e.add(header);
        e.add(chunk);
        e.add(tail);
        e.finish();
        offset += n;
        ++stats_.fragments_sent;
    }
    ++stats_.messages_sent;
    return {};
}

void Framing::receive(std::span<const std::uint8_t> bytes,
                      const std::function<void(std::span<const std::uint8_t>)>& deliver) {
    // A frame never holds more than one whole message: longer is noise.
    const auto limit = std::max(options_.frame_size, frame_bytes(options_.max_message + header_size));
    for (const auto b : bytes) {
        if (b == delimiter) {
            // A delimiter ends the open frame, or starts one: a receiver that
            // starts in the middle finds its way at the next boundary.
            if (in_frame_ && !rx_.empty()) {
                frame_done(deliver);
                in_frame_ = false;
            } else {
                in_frame_ = true;
            }
            rx_.clear();
            continue;
        }
        if (!in_frame_) continue;
        if (rx_.size() >= limit) {
            in_frame_ = false;
            rx_.clear();
            continue;
        }
        rx_.push_back(b);
    }
}

void Framing::frame_done(const std::function<void(std::span<const std::uint8_t>)>& deliver) {
    if (!decode_frame(rx_, decoded_).ok()) {
        ++stats_.frames_dropped;
        return;
    }
    ++stats_.fragments_received;
    if (decoded_.size() < header_size) {
        ++stats_.messages_dropped;
        return;
    }
    const auto flags = decoded_[0], seq = decoded_[1], total = decoded_[2];
    if ((flags & flag_first) != 0) {
        message_.clear();
        assembling_ = true;
        expected_seq_ = 0;
        total_ = total;
    }
    if (!assembling_) {
        ++stats_.fragments_orphaned;  // the rest of a message already dropped
        return;
    }
    const auto drop = [&] {
        message_.clear();
        assembling_ = false;
        ++stats_.messages_dropped;
    };
    if (seq != expected_seq_ || total != total_) return drop();
    if (message_.size() + decoded_.size() - header_size > options_.max_message) return drop();
    message_.insert(message_.end(), decoded_.begin() + header_size, decoded_.end());
    ++expected_seq_;
    if ((flags & flag_last) == 0) return;
    assembling_ = false;
    ++stats_.messages_received;
    deliver(message_);
}

// --- Port ----------------------------------------------------------------------------

Port::Port(const std::string& device, int baud) {
    const auto speed = speed_of(baud);
    if (speed == B0) throw std::runtime_error("serial: " + std::to_string(baud) + " is not a standard baud rate");
    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error("serial: cannot open " + device + ": " + std::strerror(errno));
    termios t{};
    if (::tcgetattr(fd_, &t) != 0) {
        const auto why = std::string(std::strerror(errno));
        ::close(fd_);
        throw std::runtime_error("serial: " + device + " is not a terminal: " + why);
    }
    ::cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | CRTSCTS);
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    ::cfsetispeed(&t, speed);
    ::cfsetospeed(&t, speed);
    if (::tcsetattr(fd_, TCSANOW, &t) != 0) {
        const auto why = std::string(std::strerror(errno));
        ::close(fd_);
        throw std::runtime_error("serial: cannot set up " + device + ": " + why);
    }
    ::tcflush(fd_, TCIOFLUSH);  // what came before is not ours
}

Port::Port(int fd) : fd_(fd) {}

Port::~Port() {
    if (fd_ >= 0) ::close(fd_);
}

Status Port::write(std::span<const std::uint8_t> bytes) {
    while (!bytes.empty()) {
        const auto n = ::write(fd_, bytes.data(), bytes.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd_, POLLOUT, 0};
                ::poll(&p, 1, 100);
                continue;
            }
            return Status::error(module_id::serial, errno, "write");
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return {};
}

Status Port::read(std::span<std::uint8_t> buffer, std::chrono::milliseconds timeout, std::size_t& got) {
    got = 0;
    pollfd p{fd_, POLLIN, 0};
    const auto r = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (r < 0) return errno == EINTR ? Status() : Status::error(module_id::serial, errno, "poll");
    if (r == 0) return {};
    if ((p.revents & (POLLIN | POLLHUP | POLLERR)) == 0) return {};
    const auto n = ::read(fd_, buffer.data(), buffer.size());
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return {};
        return Status::error(module_id::serial, errno, "read");
    }
    if (n == 0 && (p.revents & POLLHUP) != 0) return error(EIO, "the line is gone");
    got = static_cast<std::size_t>(n);
    return {};
}

// --- Link ----------------------------------------------------------------------------

namespace {
Port open_port(const LinkOptions& o) {
    if (o.device.empty()) {
        if (o.fd < 0) throw std::invalid_argument("serial::Link: no device and no fd");
        return Port(o.fd);
    }
    return Port(o.device, o.baud);
}
}  // namespace

Link::Link(LinkOptions options)
    : options_(std::move(options)),
      device_(options_.device.empty() ? "fd:" + std::to_string(options_.fd) : options_.device),
      port_(open_port(options_)),
      tx_framing_(options_.framing),
      rx_framing_(options_.framing) {
    worker_ = std::jthread([this](std::stop_token stop) { work_loop(stop); });
    reader_ = std::jthread([this](std::stop_token stop) { read_loop(stop); });
}

Link::~Link() {
    reader_ = {};  // request stop and join, before the worker and the port go
    worker_ = {};
}

void Link::read_loop(const std::stop_token& stop) {
    trace::set_thread_name(options_.thread_name);
    std::vector<std::uint8_t> buffer(4096);
    while (!stop.stop_requested()) {
        std::size_t n = 0;
        if (auto s = port_.read(buffer, std::chrono::milliseconds(100), n); !s.ok()) {
            std::lock_guard lock(mutex_);
            connected_ = false;
            answered_.notify_all();
            return;
        }
        if (n == 0) continue;
        rx_framing_.receive(std::span(buffer).first(n), [&](std::span<const std::uint8_t> m) { on_message(m); });
        std::lock_guard lock(mutex_);
        stats_.framing = rx_framing_.stats();
    }
}

void Link::on_message(std::span<const std::uint8_t> message) {
    c2::Message m;
    if (!c2::decode_message(message, m).ok()) return;
    switch (c2::kind_of(m.header.flags)) {
        case c2::Kind::request: {
            if (!options_.on_request) return refuse(m.header, error(ENOSYS, "this end takes no requests"));
            std::lock_guard lock(mutex_);
            ++stats_.requests_in;
            if (work_.size() >= options_.queue) {
                ++stats_.requests_refused;
                // Answered from the reader: the frame is small and the line is ours.
                refuse(m.header, Status::error(module_id::tmtc, EAGAIN, "busy"));
                return;
            }
            work_.emplace_back(message.begin(), message.end());
            work_ready_.notify_one();
            return;
        }
        case c2::Kind::reply:
        case c2::Kind::ack:
        case c2::Kind::nack: {
            std::lock_guard lock(mutex_);
            auto it = pending_.find(key_of(m.header.cmd_id, m.header.seq));
            if (it == pending_.end() || it->second.done) {
                ++stats_.answers_unmatched;  // late, after a timeout, or not ours
                return;
            }
            it->second.frames.emplace_back(message.begin(), message.end());
            const bool more = c2::kind_of(m.header.flags) == c2::Kind::reply && (m.header.flags & c2::flag::more) != 0;
            if (!more) it->second.done = true;
            answered_.notify_all();
            return;
        }
        case c2::Kind::push:
            if (options_.on_push) options_.on_push(m);
            return;
        case c2::Kind::unknown: return;
    }
}

void Link::work_loop(const std::stop_token& stop) {
    trace::set_thread_name(options_.thread_name + "-work");
    const auto send = [this](std::span<const std::uint8_t> f) {
        if (auto s = this->send(f); !s.ok())
            log::warn("serial: an answer on {} was not sent: {}", device_, s.to_string());
    };
    while (true) {
        std::vector<std::uint8_t> frame;
        {
            std::unique_lock lock(mutex_);
            if (!work_ready_.wait(lock, stop, [&] { return !work_.empty(); })) return;
            frame = std::move(work_.front());
            work_.pop_front();
        }
        options_.on_request(frame, send);
    }
}

void Link::refuse(const c2::Header& request, const Status& why) {
    const auto nack = c2::nack_payload(why);
    std::uint8_t frame[c2::header_size + c2::nack_payload_size];
    std::size_t n = 0;
    const auto h = c2::make_header(c2::flag::nack, request.cmd_id, request.seq, c2::nack_payload_size);
    if (c2::encode_message(h, nack, frame, n).ok()) (void)send(std::span(frame, n));
}

Status Link::send(std::span<const std::uint8_t> frame) {
    std::lock_guard lock(tx_mutex_);
    tx_buffer_.clear();
    if (auto s = tx_framing_.encode(frame, tx_buffer_); !s.ok()) return s;
    return port_.write(tx_buffer_);
}

Status Link::request(std::uint16_t id, std::span<const std::uint8_t> payload,
                     const std::function<Status(const c2::Message&)>& on_answer) {
    if (payload.size() > c2::payload_max) return error(EMSGSIZE, "request too large");
    std::uint32_t key = 0;
    std::uint16_t seq = 0;
    {
        std::lock_guard lock(mutex_);
        if (!connected_) return error(EIO, "the line is gone");
        do {
            seq = ++seq_;
            key = key_of(id, seq);
        } while (pending_.contains(key));
        pending_[key];
    }
    std::vector<std::uint8_t> frame(c2::header_size + payload.size());
    std::size_t n = 0;
    (void)c2::encode_message(c2::make_header(c2::flag::req, id, seq, static_cast<std::uint16_t>(payload.size())),
                             payload, frame, n);
    std::vector<std::vector<std::uint8_t>> answers;
    Status result = send(std::span(frame).first(n));
    {
        std::unique_lock lock(mutex_);
        auto& p = pending_[key];
        // The timeout starts again with each part of a long answer.
        std::size_t seen = 0;
        while (result.ok() && !p.done) {
            if (!connected_) {
                result = error(EIO, "the line is gone");
            } else if (!answered_.wait_for(lock, options_.timeout,
                                           [&] { return p.done || p.frames.size() != seen || !connected_; })) {
                result = error(ETIMEDOUT, "no answer");
            }
            seen = p.frames.size();
        }
        answers = std::move(p.frames);
        pending_.erase(key);
    }
    if (!result.ok()) return result;
    for (const auto& f : answers) {
        c2::Message m;
        if (auto s = c2::decode_message(f, m); !s.ok()) return s;
        if (c2::kind_of(m.header.flags) == c2::Kind::nack) {
            Status carried;
            if (auto s = c2::nack_status(m.payload, carried); !s.ok()) return s;
            return carried;
        }
        if (auto s = on_answer(m); !s.ok()) return s;
    }
    return {};
}

Status Link::call(std::uint16_t id, const pb::Type& type, const void* req,
                  const std::function<Status(const c2::Message&)>& on_answer) {
    std::vector<std::uint8_t> payload(pb::encoded_size(type, req));
    std::size_t n = 0;
    if (auto s = pb::encode(type, req, payload, n); !s.ok()) return s;
    return request(id, std::span(payload).first(n), on_answer);
}

Status Link::push(std::uint16_t id, const pb::Type& type, const void* body) {
    std::vector<std::uint8_t> frame(c2::header_size + pb::encoded_size(type, body));
    std::size_t n = 0;
    if (auto s = pb::encode(type, body, std::span(frame).subspan(c2::header_size), n); !s.ok()) return s;
    std::size_t written = 0;
    const auto h = c2::make_header(c2::flag::push, id, 0, static_cast<std::uint16_t>(n));
    if (auto s = c2::encode_message(h, std::span(frame).subspan(c2::header_size, n), frame, written); !s.ok()) {
        return s;
    }
    return send(std::span(frame).first(written));
}

bool Link::connected() const {
    std::lock_guard lock(mutex_);
    return connected_;
}

Link::Stats Link::stats() const {
    std::lock_guard lock(mutex_);
    auto s = stats_;
    std::lock_guard tx(tx_mutex_);
    const auto t = tx_framing_.stats();
    s.framing.messages_sent = t.messages_sent;
    s.framing.fragments_sent = t.fragments_sent;
    return s;
}

}  // namespace forge::serial
