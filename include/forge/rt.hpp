#pragma once
// Real-time settings for threads, from the configuration:
//
//   [threads.rx]          # name used by app.configure_thread("rx")
//   policy = "fifo"       # other | fifo | rr | batch | idle
//   priority = 80         # 1..99 for fifo/rr (needs CAP_SYS_NICE / rtprio limit)
//   nice = 0              # for other/batch
//   cpus = [1]            # CPU affinity
//
//   [process]
//   mlockall = true       # lock memory: no page faults in real-time paths
//
// Call app.configure_thread("rx") at the start of the thread. Missing
// permissions are reported (warning + returned error), never fatal, so the
// same config works on a desktop and on the target.

#include <optional>
#include <string>
#include <vector>

namespace forge {
class Config;
}

namespace forge::rt {

struct ThreadSettings {
    std::string policy = "other";
    int priority = 0;
    std::optional<int> nice;
    std::vector<int> cpus;
};

/// Settings of [threads.<name>] (nullopt when the section is absent).
std::optional<ThreadSettings> thread_settings(const Config& cfg, const std::string& name);

/// Apply to the calling thread; returns an empty string or a description of
/// what could not be applied.
std::string apply(const ThreadSettings& s);

/// mlockall(MCL_CURRENT | MCL_FUTURE); returns an error description or "".
std::string lock_memory();

/// Current policy/priority/affinity of the calling thread, for logs.
std::string describe_current_thread();

}  // namespace forge::rt
