#pragma once
// The FPGA's capture core through UIO (fpga/README.md has the design).
//
// /dev/uioN has two maps: map0 the core's registers (AXI-Lite), map1 the
// ring buffer in DDR the core writes its words into (reserved memory). The
// core counts the words it wrote (WR_INDEX); this side tells it how far it
// read (RD_INDEX), so the core never overwrites what was not read: when the
// buffer is full it drops words, counts them (LOST) and sends a gap word.
//
// Written against the register map of fpga/README.md; not run on hardware yet.

#include "source.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace la {

namespace reg {
constexpr std::uint32_t ID = 0x00;         ///< RO 0x4C41xxxx ("LA" + version)
constexpr std::uint32_t CTRL = 0x04;       ///< bit0 enable, bit1 clear (self-clearing), bit2 irq enable
constexpr std::uint32_t STATUS = 0x08;     ///< bit0 running, bit1 overflow (sticky), bit2 fifo overflow
constexpr std::uint32_t DIVIDER = 0x0C;    ///< sample tick = clock / (DIVIDER + 1)
constexpr std::uint32_t GLITCH = 0x10;     ///< ignore pulses shorter than this many ticks
constexpr std::uint32_t HEARTBEAT = 0x14;  ///< a time word at least every N ticks
constexpr std::uint32_t BUF_BASE = 0x18;   ///< physical address of the ring buffer
constexpr std::uint32_t BUF_WORDS = 0x1C;  ///< its size in words (a power of two)
constexpr std::uint32_t WR_INDEX = 0x20;   ///< RO words written since enable (wraps at 2^32)
constexpr std::uint32_t RD_INDEX = 0x24;   ///< RW words read by software
constexpr std::uint32_t LOST = 0x28;       ///< RO words dropped since enable
constexpr std::uint32_t LEVELS = 0x2C;     ///< RO the inputs now (bits 3..0)
constexpr std::uint32_t THRESHOLD = 0x30;  ///< comparator threshold PWM, 0..4095 = 0..3.3 V
constexpr std::uint32_t IRQ_WORDS = 0x34;  ///< interrupt after this many words (or 1 ms)
constexpr std::uint32_t CLOCK_HZ = 0x38;   ///< RO the clock the divider divides
constexpr std::uint32_t kIdMagic = 0x4C410000;
}  // namespace reg

class UioSource : public Source {
public:
    /// `device`: "/dev/uio0", or the UIO name from the device tree ("logic-analyzer").
    explicit UioSource(std::string device);
    ~UioSource() override;

    std::string name() const override {
        std::lock_guard lock(info_mutex_);
        return "uio:" + path_;
    }
    double base_clock_hz() const override {
        std::lock_guard lock(info_mutex_);
        return clock_hz_;
    }
    void start(const SourceConfig& config) override;
    void stop() override;
    std::size_t read(std::vector<std::uint32_t>& out, std::chrono::milliseconds timeout) override;
    SourceStats stats() const override;
    forge::json info() const override;

private:
    void open();
    void close();
    std::uint32_t rd(std::uint32_t offset) const;
    void wr(std::uint32_t offset, std::uint32_t value);

    // open() runs on the engine thread at the first start; name() and info()
    // are asked from others: the fields open() sets are read under this lock.
    mutable std::mutex info_mutex_;
    std::string device_, path_;
    int fd_ = -1;
    volatile std::uint32_t* regs_ = nullptr;
    std::size_t regs_size_ = 0;
    volatile const std::uint32_t* buf_ = nullptr;
    std::size_t buf_size_ = 0;
    std::uint64_t buf_phys_ = 0;
    std::uint32_t buf_words_ = 0;
    std::uint32_t read_index_ = 0;
    double clock_hz_ = 100e6;
    std::uint32_t id_ = 0;
    bool running_ = false;
    SourceStats stats_;
};

}  // namespace la
