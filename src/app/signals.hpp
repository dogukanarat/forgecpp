#pragma once
// SIGINT, SIGTERM, SIGHUP and SIGQUIT for forge::Application (not installed):
// blocked in every thread and taken by one thread of the watcher, so a signal
// never interrupts the application's code. The thread sleeps on a signalfd.

#include <csignal>
#include <functional>
#include <thread>

namespace forge::detail {

class SignalWatcher {
public:
    SignalWatcher() = default;
    ~SignalWatcher();
    SignalWatcher(const SignalWatcher&) = delete;
    SignalWatcher& operator=(const SignalWatcher&) = delete;

    /// Block the signals for the calling thread, and so for every thread it
    /// starts: call it before the first thread. Remembers the mask it found.
    void block();
    /// Start the thread: `on_signal(signal number)` runs on it for each signal.
    void start(std::function<void(int)> on_signal);
    /// Stop and join the thread (nothing happens when it does not run).
    void stop();
    /// Put the mask back as block() found it.
    void restore();

private:
    sigset_t saved_mask_{};
    bool blocked_ = false;
    int wake_fd_ = -1;  // eventfd: stop() wakes the thread's poll
    std::thread thread_;
};

}  // namespace forge::detail
