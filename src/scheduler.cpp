#include "forge/scheduler.hpp"

#include "forge/log.hpp"

#include <stdexcept>

namespace forge {

Scheduler::Scheduler(Executor executor) : executor_(std::move(executor)) {
    if (!executor_) {
        executor_ = [](const Task& t) { t(); };
    }
    thread_ = std::thread([this] { run(); });
}

Scheduler::Scheduler(ManualClock, Executor executor) : executor_(std::move(executor)), manual_(true) {
    if (!executor_) {
        executor_ = [](const Task& t) { t(); };
    }
    // No timer thread: time moves with advance().
}

Scheduler::~Scheduler() { stop(); }

Scheduler::Clock::time_point Scheduler::now() const {
    if (!manual_) return Clock::now();
    std::lock_guard lock(mutex_);
    return manual_now_;
}

std::size_t Scheduler::advance(Clock::duration d) {
    if (!manual_) throw std::logic_error("Scheduler::advance() needs the manual clock");
    std::size_t ran = 0;
    std::unique_lock lock(mutex_);
    const auto target = manual_now_ + d;
    for (;;) {
        Task task;
        Id id = 0;
        Clock::time_point due;
        if (stopping_ || !take_due(target, task, id, due)) break;
        if (due > manual_now_) manual_now_ = due;  // tasks see the time they were due at
        lock.unlock();
        execute(id, std::move(task));
        ++ran;
        lock.lock();
    }
    manual_now_ = target;
    return ran;
}

std::optional<Scheduler::Clock::time_point> Scheduler::next_due() const {
    std::lock_guard lock(mutex_);
    for (const auto& [due, id] : queue_) {
        if (entries_.count(id)) return due;  // cancelled ones stay queued until reached
    }
    return std::nullopt;
}

Scheduler::Id Scheduler::after(Clock::duration delay, Task task) {
    return schedule(now() + delay, Clock::duration::zero(), std::move(task));
}

Scheduler::Id Scheduler::at(Clock::time_point when, Task task) {
    return schedule(when, Clock::duration::zero(), std::move(task));
}

Scheduler::Id Scheduler::every(Clock::duration period, Task task, bool run_immediately) {
    if (period <= Clock::duration::zero()) period = std::chrono::milliseconds(1);
    const auto t = now();
    const auto first = run_immediately ? t : t + period;
    return schedule(first, period, std::move(task));
}

Scheduler::Id Scheduler::schedule(Clock::time_point due, Clock::duration period, Task task) {
    Id id;
    {
        std::lock_guard lock(mutex_);
        sweep_fired();
        id = next_id_++;
        auto state = std::make_shared<State>();
        state->task = std::move(task);
        entries_.emplace(id, Entry{due, period, std::move(state)});
        queue_.emplace(due, id);
    }
    cv_.notify_one();
    return id;
}

bool Scheduler::cancel(Id id) {
    std::lock_guard lock(mutex_);
    sweep_fired();
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    it->second.state->alive = false;  // a task already handed to the executor does nothing
    if (!it->second.fired) {
        auto range = queue_.equal_range(it->second.due);
        for (auto q = range.first; q != range.second; ++q) {
            if (q->second == id) {
                queue_.erase(q);
                break;
            }
        }
    }
    entries_.erase(it);
    return true;
}

void Scheduler::sweep_fired() {
    if (fired_.empty()) return;
    std::erase_if(fired_, [this](Id id) {
        auto it = entries_.find(id);
        if (it == entries_.end()) return true;
        if (!it->second.state->done) return false;
        entries_.erase(it);
        return true;
    });
}

void Scheduler::clear_entries() {
    for (auto& [id, e] : entries_) e.state->alive = false;
    entries_.clear();
    queue_.clear();
    fired_.clear();
}

void Scheduler::cancel_all() {
    std::lock_guard lock(mutex_);
    clear_entries();
}

std::size_t Scheduler::pending() const {
    std::lock_guard lock(mutex_);
    std::size_t n = entries_.size();
    for (const auto id : fired_) {  // handed over and already run: not pending
        auto it = entries_.find(id);
        if (it != entries_.end() && it->second.state->done) --n;
    }
    return n;
}

void Scheduler::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        clear_entries();
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

bool Scheduler::take_due(Clock::time_point now, Task& task, Id& id, Clock::time_point& due) {
    while (!queue_.empty() && queue_.begin()->first <= now) {
        due = queue_.begin()->first;
        id = queue_.begin()->second;
        queue_.erase(queue_.begin());
        auto it = entries_.find(id);
        if (it == entries_.end()) continue;
        if (it->second.period > Clock::duration::zero()) {
            // Share one task instance between ticks so mutable state persists.
            // cancel() clears `alive`: a tick already handed over does nothing.
            task = [s = it->second.state] {
                if (s->alive) s->task();
            };
            auto next = it->second.due + it->second.period;
            // Real clock: if we fell behind, skip missed ticks instead of bursting.
            // Manual clock: every tick runs (time jumps are deliberate).
            if (!manual_ && next <= now) {
                const auto behind = (now - next) / it->second.period + 1;
                next += it->second.period * behind;
            }
            it->second.due = next;
            queue_.emplace(next, id);
        } else {
            // The entry stays until the task ran, so cancel() still finds it
            // while the task waits in the executor's queue.
            it->second.fired = true;
            fired_.push_back(id);
            task = [s = it->second.state] {
                if (!s->alive) return;
                struct Done {  // also when the task throws
                    const std::shared_ptr<State>& s;
                    ~Done() { s->done = true; }
                } done{s};
                s->task();
            };
        }
        return true;
    }
    return false;
}

void Scheduler::execute(Id id, Task task) {
    try {
        executor_(std::move(task));
    } catch (const std::exception& e) {
        log::error("scheduler: task {} threw: {}", id, e.what());
    } catch (...) {
        log::error("scheduler: task {} threw an unknown exception", id);
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
        Task task;
        Id id = 0;
        Clock::time_point when;
        if (!take_due(Clock::now(), task, id, when)) continue;
        lock.unlock();
        execute(id, std::move(task));
        lock.lock();
    }
}

}  // namespace forge
