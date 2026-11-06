#include "forge/scheduler.hpp"

#include "forge/log.hpp"

namespace forge {

Scheduler::Scheduler(Executor executor) : executor_(std::move(executor)) {
    if (!executor_) {
        executor_ = [](Task t) { t(); };
    }
    thread_ = std::thread([this] { run(); });
}

Scheduler::~Scheduler() { stop(); }

Scheduler::Id Scheduler::after(Clock::duration delay, Task task) {
    return schedule(Clock::now() + delay, Clock::duration::zero(), std::move(task));
}

Scheduler::Id Scheduler::at(Clock::time_point when, Task task) {
    return schedule(when, Clock::duration::zero(), std::move(task));
}

Scheduler::Id Scheduler::every(Clock::duration period, Task task, bool run_immediately) {
    if (period <= Clock::duration::zero()) period = std::chrono::milliseconds(1);
    const auto first = run_immediately ? Clock::now() : Clock::now() + period;
    return schedule(first, period, std::move(task));
}

Scheduler::Id Scheduler::schedule(Clock::time_point due, Clock::duration period, Task task) {
    Id id;
    {
        std::lock_guard lock(mutex_);
        id = next_id_++;
        entries_.emplace(id, Entry{due, period, std::make_shared<Task>(std::move(task))});
        queue_.emplace(due, id);
    }
    cv_.notify_one();
    return id;
}

bool Scheduler::cancel(Id id) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    auto range = queue_.equal_range(it->second.due);
    for (auto q = range.first; q != range.second; ++q) {
        if (q->second == id) {
            queue_.erase(q);
            break;
        }
    }
    entries_.erase(it);
    return true;
}

void Scheduler::cancel_all() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    queue_.clear();
}

std::size_t Scheduler::pending() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

void Scheduler::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        entries_.clear();
        queue_.clear();
    }
    cv_.notify_all();
    std::lock_guard join_lock(join_mutex_);
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) {
            thread_.detach();  // stop() called from a task running on the timer thread
        } else {
            thread_.join();
        }
    }
}

void Scheduler::run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
        if (queue_.empty()) {
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            continue;
        }
        const auto due = queue_.begin()->first;
        if (Clock::now() < due) {
            cv_.wait_until(lock, due);
            continue;
        }

        const Id id = queue_.begin()->second;
        queue_.erase(queue_.begin());
        auto it = entries_.find(id);
        if (it == entries_.end()) continue;

        Task task;
        if (it->second.period > Clock::duration::zero()) {
            // Share one task instance between ticks so mutable state persists.
            task = [shared = it->second.task] { (*shared)(); };
            auto next = it->second.due + it->second.period;
            const auto now = Clock::now();
            if (next <= now) {
                // We fell behind: skip missed ticks instead of bursting.
                const auto behind = (now - next) / it->second.period + 1;
                next += it->second.period * behind;
            }
            it->second.due = next;
            queue_.emplace(next, id);
        } else {
            task = [shared = std::move(it->second.task)] { (*shared)(); };
            entries_.erase(it);
        }

        lock.unlock();
        try {
            executor_(std::move(task));
        } catch (const std::exception& e) {
            log::error("scheduler: task {} threw: {}", id, e.what());
        } catch (...) {
            log::error("scheduler: task {} threw an unknown exception", id);
        }
        lock.lock();
    }
}

}  // namespace forge
