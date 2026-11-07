#pragma once
// Linux process helpers for forge-hub: whether a process still lives, its CPU
// and memory use, and running systemctl.

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forgehub {

/// Start time of the process in clock ticks since boot (0 = no such process).
/// Together with the pid it identifies a process even after pid reuse.
std::uint64_t start_ticks(int pid);

/// The process exists, is not a zombie and is the one that started at `ticks`.
bool alive(int pid, std::uint64_t ticks);

struct Usage {
    std::uint64_t cpu_ticks = 0;  ///< user + system
    std::uint64_t rss_bytes = 0;
};
std::optional<Usage> usage(int pid);
long ticks_per_second();

/// Current boot id (changes on every boot); the file can be replaced in tests.
std::string boot_id(const std::string& file = "/proc/sys/kernel/random/boot_id");

/// Run a helper program (systemctl) and wait at most `timeout`.
struct CommandResult {
    int exit_code = -1;  ///< -1: could not run or timed out
    std::string output;
};
CommandResult run_command(const std::vector<std::string>& argv, std::chrono::milliseconds timeout);

/// systemd unit names the hub is willing to pass to systemctl.
bool valid_unit_name(const std::string& unit);

}  // namespace forgehub
