#include "main_loop.hpp"

#include "forge/config.hpp"
#include "forge/error.hpp"
#include "forge/http.hpp"
#include "forge/log.hpp"
#include "forge/metrics.hpp"
#include "forge/status.hpp"
#include "forge/trace.hpp"

#include <algorithm>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace forge::detail {
namespace {

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// At most one warning of a kind every 10 s: a stuck loop must not flood the log.
bool due(std::atomic<std::int64_t>& last) {
    const auto now_s = steady_ms() / 1000;
    return now_s - last.exchange(now_s) >= 10;
}

/// An exception on its way from the loop to the thread waiting in invoke():
/// described on the loop, thrown anew on the waiting thread.
struct LoopError {
    enum Kind { none, error, config, invalid_argument, out_of_range, runtime, stopped };
    Kind kind = none;
    int status = 0;
    std::string message;
    json body;

    /// Describe the exception being handled (call inside a catch block).
    void capture() noexcept {
        try {
            throw;
        } catch (const Error& e) {  // forge::Error and http::HttpError
            kind = error;
            status = e.status();
            message = e.what();
            body = e.details();
        } catch (const ConfigError& e) {
            kind = config;
            message = e.what();
        } catch (const std::invalid_argument& e) {
            kind = invalid_argument;
            message = e.what();
        } catch (const std::out_of_range& e) {
            kind = out_of_range;
            message = e.what();
        } catch (const std::exception& e) {
            kind = runtime;
            message = e.what();
        } catch (...) {
            kind = runtime;
            message = "unknown exception";
        }
    }

    [[noreturn]] void rethrow() const {
        switch (kind) {
            // an http::HttpError is a forge::Error too: either catch works
            case error: throw http::HttpError(status, message, body);
            case config: throw ConfigError(message);
            case invalid_argument: throw std::invalid_argument(message);
            case out_of_range: throw std::out_of_range(message);
            default: throw std::runtime_error(message);
        }
    }
};

void run_task(const MainLoop::Task& task, const char* when) {
    try {
        task();
    } catch (const std::exception& e) {
        log::error("unhandled exception in a main loop task{}: {}", when, e.what());
    } catch (...) {
        log::error("unhandled unknown exception in a main loop task{}", when);
    }
}

}  // namespace

void MainLoop::configure(int request_timeout_ms, int slow_task_ms, int max_pending_tasks) {
    request_timeout_ms_ = request_timeout_ms;
    slow_task_ms_ = slow_task_ms;
    max_pending_tasks_ = max_pending_tasks;
}

bool MainLoop::post(Task task) {
    if (!task) return false;
    // Lag: the time from post() to execution (a busy loop shows up here first).
    static auto& lag = metrics::registry().histogram("forge.loop.lag_seconds", metrics::latency_buckets(),
                                                     "Delay between post() and execution on the main loop", "s");
    static auto& busy_us =
        metrics::registry().counter("forge.loop.busy_us", "Time spent running main loop tasks", "us");
    static auto& slow = metrics::registry().counter("forge.loop.slow_tasks",
                                                    "Main loop tasks that took longer than [loop].slow_task_ms");
    static auto& dropped = metrics::registry().counter(
        "forge.loop.dropped", "Tasks refused because the main loop's queue was full ([loop].max_pending_tasks)");

    const auto limit = static_cast<std::size_t>(std::max(max_pending_tasks_.load(std::memory_order_relaxed), 1));
    if (tasks_.size_approx() >= limit) {
        dropped.inc();
        static std::atomic<std::int64_t> last_warning{0};
        if (due(last_warning)) {
            log::error(
                "main loop queue full ({} tasks waiting): dropping tasks (forge.loop.dropped); the loop is "
                "stuck or far behind",
                limit);
        }
        return false;
    }
    return tasks_.push([this, t = std::move(task), queued = std::chrono::steady_clock::now()] {
        const auto start = std::chrono::steady_clock::now();
        lag.observe(std::chrono::duration<double>(start - queued).count());
        FORGE_TRACE_SCOPE("loop.task", "forge");
        t();
        const auto took =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
        busy_us.inc(static_cast<std::uint64_t>(took.count()));
        // Everything else waits while a task runs: point at it.
        const int limit_ms = slow_task_ms_.load(std::memory_order_relaxed);
        if (limit_ms > 0 && took > std::chrono::milliseconds(limit_ms)) {
            slow.inc();
            static std::atomic<std::int64_t> last_warning{0};
            if (due(last_warning)) {
                log::warn(
                    "a main loop task took {} ms (> [loop].slow_task_ms = {}): everything else waited; move "
                    "heavy work off the loop with background() (forge.loop.slow_tasks, trace \"loop.task\")",
                    took.count() / 1000, limit_ms);
            }
        }
    });
}

void MainLoop::invoke(Deadline deadline, const std::function<void()>& run) {
    enum class Phase { pending, running, done, abandoned };
    struct Waiter {
        std::mutex mutex;
        std::condition_variable cv;
        Phase phase = Phase::pending;
        LoopError error;
    };
    // Finishes the wait even when the task is discarded (the application stops).
    struct Guard {
        explicit Guard(std::shared_ptr<Waiter> waiter) : w(std::move(waiter)) {}
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        std::shared_ptr<Waiter> w;
        ~Guard() {
            bool waiting;
            {
                std::lock_guard lock(w->mutex);
                waiting = w->phase == Phase::pending || w->phase == Phase::running;
                if (waiting) {
                    w->error.kind = LoopError::stopped;
                    w->error.message = "the application stopped before the task ran";
                    w->phase = Phase::done;
                }
            }
            if (waiting) w->cv.notify_all();
        }
    };
    auto w = std::make_shared<Waiter>();
    auto guard = std::make_shared<Guard>(w);
    // `run` refers to the caller's frame: it is only called while the caller
    // waits (phase running), never after the caller gave up (abandoned).
    const bool posted = post([w, &run, guard] {
        {
            std::lock_guard lock(w->mutex);
            if (w->phase == Phase::abandoned) return;  // the caller gave up: do not run
            w->phase = Phase::running;
        }
        LoopError error;
        try {
            run();
        } catch (...) {
            error.capture();
        }
        {
            std::lock_guard lock(w->mutex);
            w->error = std::move(error);
            w->phase = Phase::done;
        }
        w->cv.notify_all();
    });
    guard.reset();
    if (!posted) busy();  // the queue is full: nothing was queued, nothing to wait for
    std::unique_lock lock(w->mutex);
    const auto finished = [&] { return w->phase == Phase::done; };
    if (deadline && !w->cv.wait_until(lock, *deadline, finished) && w->phase == Phase::pending) {
        w->phase = Phase::abandoned;
        lock.unlock();
        busy();
    }
    w->cv.wait(lock, finished);
    if (w->error.kind != LoopError::none) w->error.rethrow();
}

void MainLoop::busy() const {
    static auto& timeouts =
        metrics::registry().counter("forge.control.timeouts", "Requests that gave up waiting for a busy main loop");
    timeouts.inc();
    const auto age = age_ms();
    throw Error(503, std::format("main loop busy (last heartbeat {} ms ago)", age),
                json{{"error", "main loop busy"}, {"loop_age_ms", age}, {"timeout_ms", request_timeout_ms_.load()}})
        .module(module_id::tmtc);  // forgecpp refuses, not the command
}

void MainLoop::run() {
    while (!stopping()) {
        // stop() pushes a task, so the wait only ends when there is something to do.
        auto task = tasks_.pop_for(std::chrono::seconds(5));
        if (task && *task) run_task(*task, "");
    }
}

void MainLoop::stop() {
    stopping_ = true;
    tasks_.push([] {});  // wake run()
}

void MainLoop::drain_while(const std::function<void()>& blocking) {
    std::atomic<bool> done{false};
    std::thread helper([&] {
        try {
            blocking();
        } catch (const std::exception& e) {
            log::error("shutdown step failed: {}", e.what());
        }
        done = true;
        tasks_.push([] {});
    });
    while (!done.load()) {
        auto task = tasks_.pop_for(std::chrono::milliseconds(50));
        if (task && *task) run_task(*task, " during shutdown");
    }
    helper.join();
}

void MainLoop::discard_pending() {
    while (tasks_.try_pop()) {
    }
}

void MainLoop::beat() { last_beat_ms_ = steady_ms(); }

std::int64_t MainLoop::age_ms() const {
    if (!track_age_) return 0;
    const auto last = last_beat_ms_.load();
    if (last <= 0) return 0;
    return std::max<std::int64_t>(steady_ms() - last, 0);
}

}  // namespace forge::detail
