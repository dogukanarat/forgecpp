#include "forge/gpio.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace forge::gpio {
namespace {

[[noreturn]] void fail(const std::string& what, int err) {
    throw std::runtime_error("gpio: " + what + ": " + std::strerror(err));
}

bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool is_sim(const std::string& chip) { return chip == "sim" || chip.rfind("sim:", 0) == 0; }

// --- the kernel's character device ---------------------------------------------------

class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) ::close(fd_);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }

private:
    int fd_;
};

std::string chip_label(int fd) {
    gpiochip_info info{};
    if (::ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info) != 0) return {};
    return info.label;
}

/// "/dev/gpiochip0", "gpiochip0" or a chip's label.
Fd open_chip(const std::string& chip) {
    if (chip.empty()) throw std::runtime_error("gpio: no chip named");
    if (chip.front() == '/') {
        Fd fd(::open(chip.c_str(), O_RDWR | O_CLOEXEC));
        if (fd.get() < 0) fail("cannot open " + chip, errno);
        return Fd(fd.release());
    }
    const auto dev = "/dev/" + chip;
    if (int fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC); fd >= 0) return Fd(fd);
    // Not a device name: look for a chip with this label.
    if (DIR* d = ::opendir("/dev")) {
        while (auto* e = ::readdir(d)) {
            if (std::strncmp(e->d_name, "gpiochip", 8) != 0) continue;
            Fd fd(::open(("/dev/" + std::string(e->d_name)).c_str(), O_RDWR | O_CLOEXEC));
            if (fd.get() >= 0 && chip_label(fd.get()) == chip) {
                ::closedir(d);
                return Fd(fd.release());
            }
        }
        ::closedir(d);
    }
    throw std::runtime_error("gpio: no chip " + chip + " (neither /dev/" + chip + " nor a chip with that label)");
}

/// A line's offset on the chip: its number, or the line with that name.
std::uint32_t find_line(int chip_fd, const std::string& chip, const std::string& line) {
    gpiochip_info info{};
    if (::ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &info) != 0) fail("cannot read " + chip, errno);
    if (is_number(line)) {
        const auto offset = static_cast<std::uint32_t>(std::stoul(line));
        if (offset >= info.lines) {
            throw std::runtime_error("gpio: " + chip + " has " + std::to_string(info.lines) + " lines, not " + line);
        }
        return offset;
    }
    for (std::uint32_t i = 0; i < info.lines; ++i) {
        gpio_v2_line_info li{};
        li.offset = i;
        if (::ioctl(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0 && line == li.name) return i;
    }
    throw std::runtime_error("gpio: no line named " + line + " on " + chip);
}

std::string consumer_of(int chip_fd, std::uint32_t offset) {
    gpio_v2_line_info li{};
    li.offset = offset;
    if (::ioctl(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) != 0 || li.consumer[0] == '\0') return {};
    return li.consumer;
}

class KernelLine final : public detail::Line {
public:
    KernelLine(const std::string& chip, const std::string& line, std::uint64_t flags, const std::string& consumer,
               std::optional<bool> initial, std::chrono::microseconds debounce) {
        Fd chip_fd = open_chip(chip);
        const auto offset = find_line(chip_fd.get(), chip, line);
        gpio_v2_line_request req{};
        req.offsets[0] = offset;
        req.num_lines = 1;
        std::strncpy(req.consumer, consumer.c_str(), sizeof(req.consumer) - 1);
        req.config.flags = flags;
        if (initial) {
            auto& a = req.config.attrs[req.config.num_attrs++];
            a.attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
            a.attr.values = *initial ? 1 : 0;
            a.mask = 1;
        }
        if (debounce.count() > 0) {
            auto& a = req.config.attrs[req.config.num_attrs++];
            a.attr.id = GPIO_V2_LINE_ATTR_ID_DEBOUNCE;
            a.attr.debounce_period_us = static_cast<std::uint32_t>(debounce.count());
            a.mask = 1;
        }
        if (::ioctl(chip_fd.get(), GPIO_V2_GET_LINE_IOCTL, &req) != 0) {
            const int err = errno;
            if (err == EBUSY) {
                const auto by = consumer_of(chip_fd.get(), offset);
                throw std::runtime_error("gpio: " + chip + " line " + line + " is in use" +
                                         (by.empty() ? "" : " by " + by));
            }
            fail("cannot request " + chip + " line " + line, err);
        }
        fd_ = req.fd;
    }
    ~KernelLine() override {
        if (fd_ >= 0) ::close(fd_);
    }

    bool get() const override {
        gpio_v2_line_values v{};
        v.mask = 1;
        if (::ioctl(fd_, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) != 0) fail("cannot read a line", errno);
        return (v.bits & 1) != 0;
    }

    void set(bool value) override {
        gpio_v2_line_values v{};
        v.mask = 1;
        v.bits = value ? 1 : 0;
        if (::ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) != 0) fail("cannot set a line", errno);
    }

    std::optional<Event> wait(std::chrono::milliseconds timeout) override {
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, static_cast<int>(timeout.count()));
        if (r <= 0 || (p.revents & POLLIN) == 0) return std::nullopt;  // timeout or EINTR
        gpio_v2_line_event e{};
        if (::read(fd_, &e, sizeof(e)) != static_cast<ssize_t>(sizeof(e))) return std::nullopt;
        return Event{e.id == GPIO_V2_LINE_EVENT_RISING_EDGE, std::chrono::nanoseconds(e.timestamp_ns), e.line_seqno};
    }

private:
    int fd_ = -1;
};

std::uint64_t edge_flags(Edge edge) {
    switch (edge) {
        case Edge::none: return 0;
        case Edge::rising: return GPIO_V2_LINE_FLAG_EDGE_RISING;
        case Edge::falling: return GPIO_V2_LINE_FLAG_EDGE_FALLING;
        case Edge::both: return GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING;
    }
    return 0;
}

std::uint64_t bias_flags(Bias bias) {
    switch (bias) {
        case Bias::as_is: return 0;
        case Bias::disabled: return GPIO_V2_LINE_FLAG_BIAS_DISABLED;
        case Bias::pull_up: return GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
        case Bias::pull_down: return GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
    }
    return 0;
}

std::uint64_t drive_flags(Drive drive) {
    switch (drive) {
        case Drive::push_pull: return 0;
        case Drive::open_drain: return GPIO_V2_LINE_FLAG_OPEN_DRAIN;
        case Drive::open_source: return GPIO_V2_LINE_FLAG_OPEN_SOURCE;
    }
    return 0;
}

// --- simulated chips --------------------------------------------------------------------

class SimInput;

struct SimLine {
    bool high = false;
    bool requested = false;
    SimInput* input = nullptr;  // the Input that holds the line, if one does
    std::uint32_t seqno = 0;
};

struct SimChips {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<std::string, SimLine> lines;  // "<chip>/<line>"

    static SimChips& get() {
        static SimChips chips;
        return chips;
    }
    static std::string key(const std::string& chip, const std::string& line) { return chip + "/" + line; }
};

class SimRequest : public detail::Line {
public:
    SimRequest(const std::string& chip, const std::string& line) : key_(SimChips::key(chip, line)) {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        auto& l = c.lines[key_];
        if (l.requested) throw std::runtime_error("gpio: " + chip + " line " + line + " is in use");
        l.requested = true;
    }
    ~SimRequest() override {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        auto& l = c.lines[key_];
        l.requested = false;
        l.input = nullptr;
    }

protected:
    std::string key_;
};

class SimOutput final : public SimRequest {
public:
    SimOutput(const std::string& chip, const std::string& line, const OutputOptions& o)
        : SimRequest(chip, line), active_low_(o.active_low) {
        set(o.initial);
    }
    bool get() const override {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        return c.lines[key_].high != active_low_;
    }
    void set(bool value) override {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        c.lines[key_].high = value != active_low_;
    }
    std::optional<Event> wait(std::chrono::milliseconds) override { return std::nullopt; }

private:
    bool active_low_;
};

class SimInput final : public SimRequest {
public:
    SimInput(const std::string& chip, const std::string& line, const InputOptions& o)
        : SimRequest(chip, line), active_low_(o.active_low), edge_(o.edge) {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        auto& l = c.lines[key_];
        if (o.bias == Bias::pull_up) l.high = true;
        if (o.bias == Bias::pull_down) l.high = false;
        l.input = this;
    }
    ~SimInput() override {
        // Before the events go: drive() must not reach this object any more.
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        c.lines[key_].input = nullptr;
    }
    bool get() const override {
        auto& c = SimChips::get();
        std::lock_guard lock(c.mutex);
        return c.lines[key_].high != active_low_;
    }
    void set(bool) override { throw std::logic_error("gpio: an input cannot be set"); }
    std::optional<Event> wait(std::chrono::milliseconds timeout) override {
        auto& c = SimChips::get();
        std::unique_lock lock(c.mutex);
        if (!c.changed.wait_for(lock, timeout, [&] { return !events_.empty(); })) return std::nullopt;
        auto e = events_.front();
        events_.pop_front();
        return e;
    }

    /// With the chips' mutex held: the physical level went to `high`.
    void edge(bool high, std::uint32_t seqno) {
        const bool rising = high != active_low_;
        if ((rising && (edge_ == Edge::rising || edge_ == Edge::both)) ||
            (!rising && (edge_ == Edge::falling || edge_ == Edge::both))) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            events_.push_back(Event{rising, std::chrono::duration_cast<std::chrono::nanoseconds>(now), seqno});
        }
    }

private:
    bool active_low_;
    Edge edge_;
    std::deque<Event> events_;  // the chips' mutex
};

}  // namespace

// --- Output, Input ---------------------------------------------------------------------

Output::Output(const std::string& chip, const std::string& line, OutputOptions options) : name_(chip + "/" + line) {
    if (is_sim(chip)) {
        line_ = std::make_unique<SimOutput>(chip, line, options);
        return;
    }
    const auto flags =
        GPIO_V2_LINE_FLAG_OUTPUT | drive_flags(options.drive) | (options.active_low ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
    line_ = std::make_unique<KernelLine>(chip, line, flags, options.consumer, options.initial,
                                         std::chrono::microseconds(0));
}

Output::~Output() = default;
Output::Output(Output&&) noexcept = default;
Output& Output::operator=(Output&&) noexcept = default;

void Output::set(bool value) { line_->set(value); }

bool Output::get() const { return line_->get(); }

Input::Input(const std::string& chip, const std::string& line, InputOptions options) : name_(chip + "/" + line) {
    if (is_sim(chip)) {
        line_ = std::make_unique<SimInput>(chip, line, options);
        return;
    }
    const auto flags = GPIO_V2_LINE_FLAG_INPUT | bias_flags(options.bias) | edge_flags(options.edge) |
                       (options.active_low ? GPIO_V2_LINE_FLAG_ACTIVE_LOW : 0);
    line_ = std::make_unique<KernelLine>(chip, line, flags, options.consumer, std::nullopt, options.debounce);
}

Input::~Input() = default;
Input::Input(Input&&) noexcept = default;
Input& Input::operator=(Input&&) noexcept = default;

bool Input::get() const { return line_->get(); }

std::optional<Event> Input::wait(std::chrono::milliseconds timeout) { return line_->wait(timeout); }

// --- sim -------------------------------------------------------------------------------

namespace sim {

void drive(const std::string& chip, const std::string& line, bool high) {
    if (!is_sim(chip)) throw std::invalid_argument("gpio::sim: " + chip + " is not a simulated chip");
    auto& c = SimChips::get();
    {
        std::lock_guard lock(c.mutex);
        auto& l = c.lines[SimChips::key(chip, line)];
        if (l.high == high) return;
        l.high = high;
        ++l.seqno;
        if (l.input) l.input->edge(high, l.seqno);
    }
    c.changed.notify_all();
}

bool level(const std::string& chip, const std::string& line) {
    auto& c = SimChips::get();
    std::lock_guard lock(c.mutex);
    return c.lines[SimChips::key(chip, line)].high;
}

bool requested(const std::string& chip, const std::string& line) {
    auto& c = SimChips::get();
    std::lock_guard lock(c.mutex);
    const auto it = c.lines.find(SimChips::key(chip, line));
    return it != c.lines.end() && it->second.requested;
}

}  // namespace sim
}  // namespace forge::gpio
