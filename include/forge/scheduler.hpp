#pragma once
// Timer service: one-shot and periodic tasks on a single timer thread.
//
// Tasks are handed to an Executor when due. By default they run on the timer
// thread itself; forge::Application installs an executor that runs them on
// the application's main loop, so timer callbacks never race with each other.
//
// Tests can drive time themselves (no timer thread, no sleeping):
//
//   forge::Scheduler s(forge::Scheduler::manual_clock);
//   s.every(1s, [&] { ++ticks; });
//   s.advance(10s);          // runs the 10 ticks now, in order, on this thread

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

namespace forge {

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;
    using Task = std::function<void()>;
    using Executor = std::function<void(Task)>;
    using Id = std::uint64_t;

    /// Tag: time only moves with advance() (tests).
    struct ManualClock {};
    static constexpr ManualClock manual_clock{};

    explicit Scheduler(Executor executor = {});
    explicit Scheduler(ManualClock, Executor executor = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    /// Run `task` once after `delay`.
    Id after(Clock::duration delay, Task task);
    /// Run `task` once at `when`.
    Id at(Clock::time_point when, Task task);
    /// Run `task` every `period` (fixed rate; missed ticks are skipped, not bursted).
    Id every(Clock::duration period, Task task, bool run_immediately = false);

    /// Cancel a task. A task that was handed to the executor but has not run
    /// yet is cancelled too (it does nothing when its turn comes), so on the
    /// executor's thread a true result means the task never runs again.
    /// Returns false if it was unknown or already finished.
    bool cancel(Id id);
    void cancel_all();

    [[nodiscard]] std::size_t pending() const;

    /// Current time of this scheduler (steady_clock, or the manual clock).
    Clock::time_point now() const;
    bool manual() const { return manual_; }
    /// Manual clock only: move time forward by `d` and run every task that
    /// falls due on the way, in order, on the calling thread (through the
    /// executor). Returns the number of tasks run.
    std::size_t advance(Clock::duration d);
    /// When the next pending task is due (nullopt: none).
    [[nodiscard]] std::optional<Clock::time_point> next_due() const;

    /// Stop the timer thread. Pending tasks are discarded.
    void stop();

private:
    /// Shared with the tasks handed to the executor: cancel() clears `alive`
    /// and a queued task then does nothing.
    struct State {
        Task task;
        std::atomic<bool> alive{true};
        std::atomic<bool> done{false};  ///< a one-shot task ran
    };
    struct Entry {
        Clock::time_point due;
        Clock::duration period{};
        std::shared_ptr<State> state;
        bool fired = false;  ///< one-shot: handed to the executor, kept until it ran
    };

    Id schedule(Clock::time_point due, Clock::duration period, Task task);
    /// Forget the one-shot entries whose task ran (lock held).
    void sweep_fired();
    /// Cancel every entry (lock held).
    void clear_entries();
    void run();
    /// Take the first task due at or before `now` (lock held); false if none.
    bool take_due(Clock::time_point now, Task& task, Id& id, Clock::time_point& due);
    void execute(Id id, Task task);

    Executor executor_;
    mutable std::mutex mutex_;
    std::mutex join_mutex_;
    std::condition_variable cv_;
    std::unordered_map<Id, Entry> entries_;
    std::multimap<Clock::time_point, Id> queue_;
    std::vector<Id> fired_;  ///< one-shot entries handed to the executor
    Id next_id_ = 1;
    bool stopping_ = false;
    bool manual_ = false;
    Clock::time_point manual_now_{};
    std::thread thread_;
};

}  // namespace forge
