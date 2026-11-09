#pragma once
// Crash reports. On SIGSEGV / SIGBUS / SIGFPE / SIGILL / SIGABRT the handler
// writes a report (signal, time, backtrace, the last log lines) to
// <[crash].dir>/<app>-crash.txt and lets the process die as before (core dumps
// still work). On the next start the application finds the report, logs it,
// raises the "forge.crashed" alarm and renames it to <app>-crash-<time>.txt.
//
//   [crash]
//   enabled = true
//   dir = "/var/lib/<app>"   # default: $STATE_DIRECTORY (systemd) or /tmp

#include <cstddef>
#include <optional>
#include <string>

namespace forge::crash {

/// Install the handlers. `dir` must exist and be writable.
void install(const std::string& app, const std::string& dir);
/// Path of a report left by a previous run (nullopt if none).
std::optional<std::string> previous_report(const std::string& app, const std::string& dir);
/// Keep the last log lines in a buffer the handler can write without allocating.
void remember_log_line(const char* data, std::size_t size);
/// Give the calling thread a stack for the handler, so that a stack overflow
/// on it is reported too. install() does it for its thread; forge::Worker and
/// Application::configure_thread() for theirs. Call it on threads of your own.
void use_alternate_stack();

}  // namespace forge::crash
