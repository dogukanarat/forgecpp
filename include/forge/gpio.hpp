#pragma once
// GPIO lines through the kernel's GPIO character device (/dev/gpiochipN,
// uAPI v2, Linux 5.10 and later): the interface libgpiod 2 wraps, used
// directly, so there is no library to depend on.
//
//   forge::gpio::Output power("/dev/gpiochip0", "PAYLOAD_EN");    // a line by its name
//   power.set(true);
//
//   forge::gpio::Input pgood("gpiochip0", "17", {.edge = forge::gpio::Edge::both});
//   if (auto e = pgood.wait(100ms)) { ... e->rising ... }          // in a worker()
//
// A chip is a path ("/dev/gpiochip0"), a name ("gpiochip0") or a label (the
// chip's driver label, `gpiodetect` shows it). A line is its name (from the
// device tree, `gpioinfo` shows it) or its offset on the chip ("17").
//
// The chip "sim" (or "sim:<name>") is a simulated chip in the process: any
// line name exists on it, outputs can be read and inputs driven with
// gpio::sim. Tests use it, and an application can default to it so that it
// runs on a PC ([gpio].chip = "sim").
//
// Values are logical: with active_low, set(true) drives the line low, and a
// rising edge is the line becoming active. Errors when a line is requested
// (no such chip or line, the line is in use) throw std::runtime_error with
// the reason.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace forge::gpio {

enum class Drive { push_pull, open_drain, open_source };
enum class Bias { as_is, disabled, pull_up, pull_down };
enum class Edge { none, rising, falling, both };

struct OutputOptions {
    bool initial = false;     ///< the value when the line is requested
    bool active_low = false;  ///< true: set(true) drives the line low
    Drive drive = Drive::push_pull;
    std::string consumer = "forge";  ///< shown by gpioinfo
};

struct InputOptions {
    Bias bias = Bias::as_is;
    bool active_low = false;
    Edge edge = Edge::none;              ///< which edges wait() reports
    std::chrono::microseconds debounce{0};  ///< 0: none (not every chip supports it)
    std::string consumer = "forge";
};

/// An edge on an input.
struct Event {
    bool rising = false;            ///< the line became active (logical value 1)
    std::chrono::nanoseconds time{};  ///< when the kernel saw it (CLOCK_MONOTONIC)
    std::uint32_t seqno = 0;        ///< counts the line's events: a gap means events were lost
};

namespace detail {
struct Line {
    virtual ~Line() = default;
    virtual bool get() const = 0;
    virtual void set(bool value) = 0;
    virtual std::optional<Event> wait(std::chrono::milliseconds timeout) = 0;
};
}  // namespace detail

/// One line driven by this program. Released when the object goes.
class Output {
public:
    Output(const std::string& chip, const std::string& line, OutputOptions options = {});
    ~Output();
    Output(Output&&) noexcept;
    Output& operator=(Output&&) noexcept;

    void set(bool value);
    bool get() const;  ///< the value it drives
    const std::string& name() const { return name_; }

private:
    std::unique_ptr<detail::Line> line_;
    std::string name_;
};

/// One line read by this program, with its edges if asked for.
class Input {
public:
    Input(const std::string& chip, const std::string& line, InputOptions options = {});
    ~Input();
    Input(Input&&) noexcept;
    Input& operator=(Input&&) noexcept;

    bool get() const;
    /// The next edge, or nothing after `timeout` (InputOptions::edge must name
    /// one). Blocks: call it from a worker(), with a timeout short enough to
    /// see the stop request.
    std::optional<Event> wait(std::chrono::milliseconds timeout);
    const std::string& name() const { return name_; }

private:
    std::unique_ptr<detail::Line> line_;
    std::string name_;
};

/// The simulated chips ("sim", "sim:<name>").
namespace sim {
/// Set the physical level of a line of a simulated chip, as the device on
/// the other end would (an Input with active_low reads the opposite).
/// Inputs that asked for that edge get it from wait().
void drive(const std::string& chip, const std::string& line, bool high);
/// The physical level of a line of a simulated chip (what an Output drives).
bool level(const std::string& chip, const std::string& line);
/// Whether a line of a simulated chip is requested by an Output or an Input.
bool requested(const std::string& chip, const std::string& line);
}  // namespace sim

}  // namespace forge::gpio
