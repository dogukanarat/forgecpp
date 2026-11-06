#pragma once
// Timer service: one-shot and periodic tasks on a single timer thread.
//
// Tasks are handed to an Executor when due. By default they run on the timer
// thread itself; forge::Application installs an executor that runs them on
// the application's main loop, so timer callbacks never race with each other.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace forge {

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;
    using Task = std::function<void()>;
    using Executor = std::function<void(Task)>;
    using Id = std::uint64_t;

    explicit Scheduler(Executor executor = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Run `task` once after `delay`.
    Id after(Clock::duration delay, Task task);
    /// Run `task` once at `when`.
    Id at(Clock::time_point when, Task task);
    /// Run `task` every `period` (fixed rate; missed ticks are skipped, not bursted).
    Id every(Clock::duration period, Task task, bool run_immediately = false);

    /// Cancel a pending task. Returns false if it was unknown or already finished.
    bool cancel(Id id);
    void cancel_all();

    std::size_t pending() const;

    /// Stop the timer thread. Pending tasks are discarded.
    void stop();

private:
    struct Entry {
        Clock::time_point due;
        Clock::duration period{};
        std::shared_ptr<Task> task;
    };

    Id schedule(Clock::time_point due, Clock::duration period, Task task);
    void run();

    Executor executor_;
    mutable std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable cv_;
    std::unordered_map<Id, Entry> entries_;
    std::multimap<Clock::time_point, Id> queue_;
    Id next_id_ = 1;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace forge
