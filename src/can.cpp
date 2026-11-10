#include "forge/can.hpp"

#include <fcntl.h>
#include <linux/can.h>
#include <linux/can/isotp.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>
#include <utility>

namespace forge::can {
namespace {

Status error(int code, const char* what) { return Status::error(module_id::can, code, what); }

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error("can: " + what + ": " + std::strerror(err));
}

unsigned interface_index(const std::string& interface) {
    if (interface.empty()) throw std::runtime_error("can: no interface named");
    const unsigned index = ::if_nametoindex(interface.c_str());
    if (index == 0) fail("no interface " + interface, errno);
    return index;
}

bool valid_fd_size(std::uint8_t size) {
    static constexpr std::uint8_t sizes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return std::find(std::begin(sizes), std::end(sizes), size) != std::end(sizes);
}

canid_t to_kernel_id(std::uint32_t id, bool extended, bool remote) {
    canid_t k = extended ? ((id & CAN_EFF_MASK) | CAN_EFF_FLAG) : (id & CAN_SFF_MASK);
    if (remote) k |= CAN_RTR_FLAG;
    return k;
}

}  // namespace

Frame Frame::make(std::uint32_t id, std::initializer_list<std::uint8_t> bytes, bool extended) {
    return make(id, std::span<const std::uint8_t>(bytes.begin(), bytes.size()), extended);
}

Frame Frame::make(std::uint32_t id, std::span<const std::uint8_t> bytes, bool extended) {
    if (bytes.size() > 8) throw std::invalid_argument("can: a classic frame carries at most 8 bytes");
    Frame f;
    f.id = id;
    f.extended = extended;
    f.size = static_cast<std::uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), f.data.begin());
    return f;
}

// --- Socket ----------------------------------------------------------------------------

Socket::Socket(const std::string& interface, SocketOptions options) : fd_frames_(options.fd) {
    const auto index = interface_index(interface);
    fd_ = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, CAN_RAW);
    if (fd_ < 0) fail("cannot open a raw CAN socket", errno);
    try {
        if (options.fd) {
            const int on = 1;
            if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) != 0) {
                fail(interface + " takes no CAN FD frames", errno);
            }
        }
        if (!options.filters.empty()) {
            std::vector<can_filter> filters;
            for (const auto& f : options.filters) {
                filters.push_back({to_kernel_id(f.id, f.extended, false),
                                   (f.extended ? (f.mask & CAN_EFF_MASK) : (f.mask & CAN_SFF_MASK)) | CAN_EFF_FLAG});
            }
            if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_FILTER, filters.data(),
                             static_cast<socklen_t>(filters.size() * sizeof(can_filter))) != 0) {
                fail("cannot set the filters on " + interface, errno);
            }
        }
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = static_cast<int>(index);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            fail("cannot bind to " + interface, errno);
    } catch (...) {
        ::close(fd_);
        throw;
    }
}

Socket::Socket(int fd, bool fd_frames) : fd_(fd), fd_frames_(fd_frames) {
    const int flags = ::fcntl(fd_, F_GETFL);
    if (flags >= 0) ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
}

Socket::~Socket() {
    if (fd_ >= 0) ::close(fd_);
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)), fd_frames_(other.fd_frames_) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = std::exchange(other.fd_, -1);
        fd_frames_ = other.fd_frames_;
    }
    return *this;
}

Status Socket::send(const Frame& frame, std::chrono::milliseconds timeout) {
    canfd_frame k{};
    std::size_t size = CAN_MTU;
    if (frame.fd) {
        if (!fd_frames_) return error(EINVAL, "the socket takes no CAN FD frames");
        if (!valid_fd_size(frame.size)) return error(EINVAL, "not a CAN FD frame size");
        size = CANFD_MTU;
    } else if (frame.size > 8) {
        return error(EINVAL, "a classic frame carries at most 8 bytes");
    }
    k.can_id = to_kernel_id(frame.id, frame.extended, frame.remote);
    k.len = frame.size;
    std::copy_n(frame.data.begin(), frame.size, k.data);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const auto n = ::write(fd_, &k, size);
        if (n == static_cast<ssize_t>(size)) return {};
        if (n >= 0) return error(EIO, "a frame was cut short");
        if (errno == EINTR) continue;
        // ENOBUFS: the interface's queue is full (nobody acknowledges on the bus).
        if (errno != EAGAIN && errno != ENOBUFS) return Status::error(module_id::can, errno, "send");
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero()) return error(ETIMEDOUT, "the bus takes no frames");
        pollfd p{fd_, POLLOUT, 0};
        ::poll(&p, 1, static_cast<int>(std::min<std::int64_t>(left.count(), 10)));
    }
}

Status Socket::receive(Frame& frame, std::chrono::milliseconds timeout) {
    pollfd p{fd_, POLLIN, 0};
    const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
    if (r < 0 && errno != EINTR) return Status::error(module_id::can, errno, "poll");
    if (r <= 0) return error(ETIMEDOUT, "no frame");
    canfd_frame k{};
    const auto n = ::read(fd_, &k, sizeof(k));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return error(ETIMEDOUT, "no frame");
        return Status::error(module_id::can, errno, "receive");
    }
    if (n != CAN_MTU && n != CANFD_MTU) return error(EBADMSG, "not a CAN frame");
    frame = Frame{};
    frame.fd = n == CANFD_MTU;
    frame.extended = (k.can_id & CAN_EFF_FLAG) != 0;
    frame.remote = (k.can_id & CAN_RTR_FLAG) != 0;
    frame.id = frame.extended ? (k.can_id & CAN_EFF_MASK) : (k.can_id & CAN_SFF_MASK);
    frame.size = std::min<std::uint8_t>(k.len, frame.fd ? 64 : 8);
    std::copy_n(k.data, frame.size, frame.data.begin());
    return {};
}

// --- ISO-TP ----------------------------------------------------------------------------

int open_isotp(const IsotpOptions& o) {
    const auto index = interface_index(o.interface);
    const int fd = ::socket(PF_CAN, SOCK_DGRAM | SOCK_CLOEXEC, CAN_ISOTP);
    if (fd < 0) fail("cannot open an ISO-TP socket (is CONFIG_CAN_ISOTP in the kernel?)", errno);
    try {
        can_isotp_options opts{};
        if (o.padding >= 0) {
            opts.flags |= CAN_ISOTP_TX_PADDING;
            opts.txpad_content = static_cast<__u8>(o.padding);
        }
        if (::setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_OPTS, &opts, sizeof(opts)) != 0) {
            fail("cannot set the ISO-TP options", errno);
        }
        if (o.fd) {
            can_isotp_ll_options ll{};
            ll.mtu = CANFD_MTU;
            ll.tx_dl = 64;
            if (::setsockopt(fd, SOL_CAN_ISOTP, CAN_ISOTP_LL_OPTS, &ll, sizeof(ll)) != 0) {
                fail(o.interface + " takes no CAN FD frames", errno);
            }
        }
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = static_cast<int>(index);
        addr.can_addr.tp.tx_id = to_kernel_id(o.tx_id, o.extended, false);
        addr.can_addr.tp.rx_id = to_kernel_id(o.rx_id, o.extended, false);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            fail("cannot bind ISO-TP on " + o.interface, errno);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    return fd;
}

std::string describe(const IsotpOptions& o) { return std::format("{} 0x{:X}/0x{:X}", o.interface, o.tx_id, o.rx_id); }

serial::LinkOptions isotp_link(const IsotpOptions& options, serial::LinkOptions base) {
    base.device.clear();
    base.fd = open_isotp(options);
    base.label = describe(options);
    base.frame_per_write = true;  // one framed piece per ISO-TP message
    return base;
}

}  // namespace forge::can
