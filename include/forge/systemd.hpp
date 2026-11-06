#pragma once
// Minimal systemd integration without linking libsystemd.
// Works with Type=notify units and WatchdogSec=; no-ops elsewhere.

#include <chrono>
#include <optional>
#include <string_view>

namespace forge::systemd {

/// Send a notification ("READY=1", "STOPPING=1", "WATCHDOG=1", "STATUS=...")
/// to $NOTIFY_SOCKET. Returns false when not running under systemd.
bool notify(std::string_view state);

/// Watchdog interval requested by systemd (WATCHDOG_USEC), if any.
std::optional<std::chrono::microseconds> watchdog_interval();

/// True when started by systemd with a notify socket.
bool available();

}  // namespace forge::systemd
