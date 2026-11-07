#pragma once
// forge-hub's view of systemd: the state and restart policy of units, and
// start / stop / restart. The real implementation runs systemctl; tests use a fake.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace forgehub {

struct UnitState {
    bool loaded = false;        ///< LoadState=loaded
    std::string active;         ///< ActiveState: active, activating, deactivating, inactive, failed
    std::string sub;            ///< SubState: running, auto-restart, dead, failed, ...
    std::string result;         ///< Result: success, exit-code, signal, core-dump, watchdog, start-limit-hit, ...
    int main_pid = 0;
    std::uint32_t n_restarts = 0;  ///< automatic restarts since the unit was last started by hand
    // restart policy
    std::string restart;           ///< Restart=
    std::uint32_t start_limit_burst = 0;
    std::uint64_t start_limit_interval_us = 0;
    std::uint64_t restart_us = 0;
    std::uint64_t watchdog_us = 0;
};

class Systemd {
public:
    virtual ~Systemd() = default;
    /// systemd unit whose main process is `pid` ("" if none).
    virtual std::string unit_of(int pid) = 0;
    /// State of the units (one query for all of them).
    virtual std::map<std::string, UnitState> show(const std::vector<std::string>& units) = 0;
    /// start, stop, restart, reset-failed; false and *error on failure.
    virtual bool action(const std::string& verb, const std::string& unit, std::string* error) = 0;
};

/// systemctl-based implementation (does nothing useful without systemd: unit_of() is always "").
std::unique_ptr<Systemd> make_systemctl();

/// systemd timespan as systemctl show prints it ("2s", "10min", "1min 30s", "100ms",
/// "infinity" = 0) in microseconds; a plain number is microseconds already.
std::uint64_t timespan_us(const std::string& text);

/// Parse `systemctl show -p ... unit...` output (blocks separated by blank lines).
std::map<std::string, UnitState> parse_show(const std::string& output);

}  // namespace forgehub
