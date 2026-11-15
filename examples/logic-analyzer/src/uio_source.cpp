#include "uio_source.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace la {
namespace {

std::string read_line(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

std::uint64_t read_number(const std::filesystem::path& p) {
    const auto s = read_line(p);
    if (s.empty()) throw std::runtime_error("uio: cannot read " + p.string());
    return std::stoull(s, nullptr, 0);
}

/// "/dev/uio3" as it is, or the uio device whose name is `device`.
std::string resolve(const std::string& device) {
    if (device.starts_with("/dev/")) return device;
    const std::filesystem::path cls = "/sys/class/uio";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(cls, ec)) {
        if (read_line(e.path() / "name") == device) return "/dev/" + e.path().filename().string();
    }
    throw std::runtime_error("uio: no device named \"" + device +
                             "\" in /sys/class/uio (device tree, uio_pdrv_genirq)");
}

}  // namespace

UioSource::UioSource(std::string device) : device_(std::move(device)) {}

UioSource::~UioSource() {
    if (running_) wr(reg::CTRL, 0);
    close();
}

void UioSource::open() {
    if (fd_ >= 0) return;
    path_ = resolve(device_);
    const std::string uio = std::filesystem::path(path_).filename().string();
    const std::filesystem::path maps = "/sys/class/uio/" + uio + "/maps";
    regs_size_ = read_number(maps / "map0" / "size");
    buf_size_ = read_number(maps / "map1" / "size");
    buf_phys_ = read_number(maps / "map1" / "addr");
    fd_ = ::open(path_.c_str(), O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error(std::format("uio: cannot open {}: {}", path_, std::strerror(errno)));
    const long page = ::sysconf(_SC_PAGESIZE);
    void* r = ::mmap(nullptr, regs_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    // map N is at offset N pages (the UIO convention)
    void* b = ::mmap(nullptr, buf_size_, PROT_READ, MAP_SHARED, fd_, page);
    if (r == MAP_FAILED || b == MAP_FAILED) {
        const int err = errno;
        if (r != MAP_FAILED) ::munmap(r, regs_size_);
        if (b != MAP_FAILED) ::munmap(b, buf_size_);
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error(std::format("uio: cannot map {}: {}", path_, std::strerror(err)));
    }
    regs_ = static_cast<volatile std::uint32_t*>(r);
    buf_ = static_cast<volatile const std::uint32_t*>(b);
    id_ = rd(reg::ID);
    if ((id_ & 0xFFFF0000u) != reg::kIdMagic) {
        const auto id = id_;
        close();
        throw std::runtime_error(std::format("uio: {} is not the logic analyzer core (ID 0x{:08X})", path_, id));
    }
    const auto clock = rd(reg::CLOCK_HZ);
    if (clock) clock_hz_ = clock;
    // The largest power of two of words that fits the buffer.
    buf_words_ = 1;
    while (static_cast<std::size_t>(buf_words_) * 2 * 4 <= buf_size_ && buf_words_ < (1u << 30)) buf_words_ *= 2;
}

void UioSource::close() {
    if (regs_) ::munmap(const_cast<std::uint32_t*>(regs_), regs_size_);
    if (buf_) ::munmap(const_cast<std::uint32_t*>(buf_), buf_size_);
    regs_ = nullptr;
    buf_ = nullptr;
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

std::uint32_t UioSource::rd(std::uint32_t offset) const { return regs_[offset / 4]; }
void UioSource::wr(std::uint32_t offset, std::uint32_t value) { regs_[offset / 4] = value; }

void UioSource::start(const SourceConfig& config) {
    open();
    const double div = clock_hz_ / config.sample_hz;
    if (div < 1 || std::abs(div - std::round(div)) > 1e-6)
        throw std::invalid_argument(
            std::format("{} Hz is not {} Hz divided by a whole number", config.sample_hz, clock_hz_));
    const auto heartbeat = config.heartbeat_ticks ? config.heartbeat_ticks
                                                  : static_cast<std::uint32_t>(std::max(1.0, config.sample_hz / 1000));
    wr(reg::CTRL, 0);
    wr(reg::CTRL, 2);  // clear: indexes, counters, sticky flags
    wr(reg::DIVIDER, static_cast<std::uint32_t>(std::lround(div)) - 1);
    wr(reg::GLITCH, config.glitch_ticks);
    wr(reg::HEARTBEAT, std::min(heartbeat, (1u << 26) - 1));
    wr(reg::BUF_BASE, static_cast<std::uint32_t>(buf_phys_));
    wr(reg::BUF_WORDS, buf_words_);
    wr(reg::RD_INDEX, 0);
    wr(reg::THRESHOLD, static_cast<std::uint32_t>(std::clamp(config.threshold_v / 3.3, 0.0, 1.0) * 4095));
    wr(reg::IRQ_WORDS, std::min<std::uint32_t>(4096, buf_words_ / 4));
    read_index_ = 0;
    stats_ = {};
    wr(reg::CTRL, 1 | 4);  // enable, interrupts
    running_ = true;
}

void UioSource::stop() {
    if (!regs_) return;
    wr(reg::CTRL, 0);
    running_ = false;
}

std::size_t UioSource::read(std::vector<std::uint32_t>& out, std::chrono::milliseconds timeout) {
    if (!running_) return 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint32_t avail = 0;
    for (;;) {
        avail = rd(reg::WR_INDEX) - read_index_;
        if (avail > 0) break;
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return 0;
        // Wait for the core's interrupt (uio_pdrv_genirq: write 1 to unmask it).
        const std::uint32_t unmask = 1;
        if (::write(fd_, &unmask, sizeof(unmask)) == sizeof(unmask)) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) > 0) {
                std::uint32_t count = 0;
                [[maybe_unused]] auto n = ::read(fd_, &count, sizeof(count));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));  // no interrupt: poll
        }
    }
    avail = std::min<std::uint32_t>(avail, 1u << 20);  // at most 4 MB per call
    const std::size_t at = out.size();
    out.resize(at + avail);
    const std::uint32_t mask = buf_words_ - 1;
    for (std::uint32_t i = 0; i < avail; ++i) out[at + i] = buf_[(read_index_ + i) & mask];
    read_index_ += avail;
    wr(reg::RD_INDEX, read_index_);  // the core may write there again
    stats_.words += avail;
    return avail;
}

SourceStats UioSource::stats() const {
    SourceStats s = stats_;
    if (regs_) {
        s.lost = rd(reg::LOST);
        s.overflows = (rd(reg::STATUS) & 2u) ? 1 : 0;
    }
    return s;
}

forge::json UioSource::info() const {
    return {{"device", path_},
            {"core_version", id_ & 0xFFFF},
            {"clock_hz", clock_hz_},
            {"buffer_bytes", static_cast<std::uint64_t>(buf_words_) * 4},
            {"buffer_address", std::format("0x{:08X}", buf_phys_)}};
}

}  // namespace la
