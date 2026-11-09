#pragma once
// The main loop of forge::Application (not installed): one thread runs every
// task posted to it, in order, so the application's state needs no locks.
//
// The queue is bounded: a loop that is stuck must not let the producers fill
// the memory. post() then refuses, and invoke() answers "main loop busy".

#include "forge/channel.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>

namespace forge::detail {

class MainLoop {
public:
    using Task = std::function<void()>;
    using Deadline = std::optional<std::chrono::steady_clock::time_point>;

    /// The calling thread is the loop's thread from now on (Application::run()).
    void bind_to_this_thread() { thread_ = std::this_thread::get_id(); }
    bool on_loop() const { return std::this_thread::get_id() == thread_.load(); }

    /// The [loop] settings (at start-up and on reload).
    void configure(int request_timeout_ms, int slow_task_ms, int max_pending_tasks);
    std::chrono::milliseconds request_timeout() const { return std::chrono::milliseconds(request_timeout_ms_.load()); }

    /// Queue `task` (any thread). False when the queue is full: the task is
    /// dropped and counted (forge.loop.dropped).
    bool post(Task task);

    /// Run `run` on the loop and wait for it (not from the loop itself). With
    /// a deadline, give up when the loop has not started it by then: busy()
    /// throws and `run` never runs. Once started it is waited for, so `run`
    /// may refer to the caller's frame. An exception of `run` is described on
    /// the loop and thrown anew here (a new object; none crosses threads).
    void invoke(Deadline deadline, const std::function<void()>& run);

    /// Counts the timeout and throws forge::Error 503 "main loop busy".
    [[noreturn]] void busy() const;

    /// Run tasks on the calling thread until stop().
    void run();
    /// Ask run() to return (any thread).
    void stop();
    bool stopping() const { return stopping_.load(); }

    /// Run `blocking` on a helper thread while this (the loop's) thread keeps
    /// running posted tasks: what `blocking` waits for may itself be waiting
    /// for the loop (a request handler in invoke()).
    void drain_while(const std::function<void()>& blocking);
    /// Drop the tasks that never ran (their promises break: no waiter hangs).
    void discard_pending();

    std::size_t queued() const { return tasks_.size_approx(); }

    /// The heartbeat: a timer on the loop calls beat(); age_ms() is how long
    /// ago that was (0 before the first one, and with track_age(false): a
    /// manual clock only beats when the test advances time).
    void beat();
    std::int64_t age_ms() const;
    void track_age(bool on) { track_age_ = on; }

private:
    Channel<Task> tasks_{256};
    std::atomic<std::thread::id> thread_{};
    std::atomic<bool> stopping_{false};
    std::atomic<int> request_timeout_ms_{2000};
    std::atomic<int> slow_task_ms_{200};
    std::atomic<int> max_pending_tasks_{10000};
    std::atomic<std::int64_t> last_beat_ms_{0};
    std::atomic<bool> track_age_{true};
};

}  // namespace forge::detail
