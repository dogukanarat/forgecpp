#pragma once
// forge::Worker: a named thread that stops when asked, for a loop that does
// not fit the worker pool (it reads a device, waits on a socket, runs for the
// life of the program).
//
//   worker_ = std::make_unique<forge::Worker>("producer", [this](std::stop_token stop) {
//       while (!stop.stop_requested()) {
//           produce();
//           forge::sleep_for(stop, 50ms);   // returns at once when stop is asked
//       }
//   });
//
// The destructor asks the thread to stop and joins it. An exception that
// leaves the body ends the thread: it is logged, and `on_error` gets it.
// In an Application, worker(name, body) does the same, applies
// [threads.<name>] (priority, CPUs) and stops the thread before on_stop().

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace forge {

/// Waits `d`, or less if `stop` is asked. True if stop was asked.
template <class Rep, class Period>
bool sleep_for(const std::stop_token& stop, std::chrono::duration<Rep, Period> d) {
    std::mutex m;
    std::condition_variable_any cv;
    std::unique_lock lock(m);
    return cv.wait_for(lock, stop, d, [] { return false; }) || stop.stop_requested();
}

class Worker {
public:
    using Body = std::function<void(std::stop_token)>;
    using OnError = std::function<void(const std::string& what)>;

    /// Starts `body` on a new thread named `name` (shown in the trace view).
    Worker(std::string name, Body body, OnError on_error = {});
    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Asks the thread to stop and waits for it. Calling it again does nothing.
    void stop();
    /// True until the body has returned (or thrown).
    bool running() const;
    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::atomic<bool> running_{true};
    std::jthread thread_;
};

}  // namespace forge
