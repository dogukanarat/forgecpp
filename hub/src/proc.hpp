#pragma once
// Linux process helpers for forge-hub: what a process is (/proc), whether it
// still lives, its CPU and memory use, and starting it again.

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forgehub {

/// Everything needed to start a process again, as the kernel reports it.
struct LaunchInfo {
    std::string exe;                ///< /proc/<pid>/exe
    std::vector<std::string> argv;  ///< /proc/<pid>/cmdline
    std::string cwd;                ///< /proc/<pid>/cwd
    std::vector<std::string> env;   ///< /proc/<pid>/environ ("KEY=value")
    uid_t uid = 0;
    gid_t gid = 0;
};

/// Read a running process' launch information; nullopt (and *error) when it is
/// gone or not readable (other user without privileges).
std::optional<LaunchInfo> read_launch(int pid, std::string* error = nullptr);

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

/// Start `info` again: same executable, arguments, directory, environment and
/// user (privileges are dropped when the hub runs as root); output goes to
/// `log_file` ("" = /dev/null). Returns the pid, or -1 with *error.
int spawn(const LaunchInfo& info, const std::string& log_file, std::string* error = nullptr);

/// Reap a child started by spawn() if it ended (non-blocking).
bool reap(int pid, int* status = nullptr);

/// Run a helper program (systemctl) and wait at most `timeout`.
struct CommandResult {
    int exit_code = -1;  ///< -1: could not run or timed out
    std::string output;
};
CommandResult run_command(const std::vector<std::string>& argv, std::chrono::milliseconds timeout);

/// systemd unit names the hub is willing to pass to systemctl.
bool valid_unit_name(const std::string& unit);

}  // namespace forgehub
