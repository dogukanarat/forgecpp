#include <forge/scheduler.hpp>
#include <forge/testing.hpp>

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

TEST_CASE("scheduler: one-shot and periodic") {
    forge::Scheduler s;
    std::atomic<int> once{0}, periodic{0};

    s.after(10ms, [&] { ++once; });
    const auto id = s.every(5ms, [&] { ++periodic; });

    CHECK(forge::testing::wait_until([&] { return once == 1 && periodic >= 3; }));
    CHECK(s.cancel(id));
    CHECK_FALSE(s.cancel(id));
    const int after_cancel = periodic;
    std::this_thread::sleep_for(30ms);
    CHECK(periodic <= after_cancel + 1);
    CHECK(once == 1);
}

TEST_CASE("scheduler: cancel before due and custom executor") {
    std::atomic<int> executed{0};
    forge::Scheduler s([&](forge::Scheduler::Task t) {
        ++executed;
        t();
    });
    std::atomic<bool> ran{false};
    const auto id = s.after(50ms, [&] { ran = true; });
    CHECK(s.pending() == 1);
    CHECK(s.cancel(id));
    s.after(1ms, [] {});
    CHECK(forge::testing::wait_until([&] { return executed == 1; }));
    std::this_thread::sleep_for(80ms);
    CHECK_FALSE(ran);
}

TEST_CASE("scheduler: cancel() also stops a task the executor has not run yet") {
    // An executor that queues (forge::Application posts timer tasks to its main
    // loop): the task is handed over, then cancelled, then the queue runs.
    std::mutex mutex;
    std::vector<forge::Scheduler::Task> queue;
    forge::Scheduler s([&](forge::Scheduler::Task t) {
        std::lock_guard lock(mutex);
        queue.push_back(std::move(t));
    });
    const auto run_queue = [&] {
        std::vector<forge::Scheduler::Task> tasks;
        {
            std::lock_guard lock(mutex);
            tasks.swap(queue);
        }
        for (auto& t : tasks) t();
        return tasks.size();
    };
    const auto queued = [&] {
        std::lock_guard lock(mutex);
        return queue.size();
    };

    int once = 0, periodic = 0;
    const auto one = s.after(1ms, [&] { ++once; });
    const auto many = s.every(1ms, [&] { ++periodic; });
    CHECK(forge::testing::wait_until([&] { return queued() >= 2; }));
    CHECK(s.cancel(one));   // handed over, not run: still known, and it never runs
    CHECK(s.cancel(many));  // the ticks in the queue do nothing
    CHECK_FALSE(s.cancel(one));
    CHECK(run_queue() >= 2);
    CHECK(once == 0);
    CHECK(periodic == 0);
    CHECK(s.pending() == 0);

    // The one that runs is forgotten afterwards.
    const auto other = s.after(1ms, [&] { ++once; });
    CHECK(forge::testing::wait_until([&] { return queued() >= 1; }));
    CHECK(s.pending() == 1);
    run_queue();
    CHECK(once == 1);
    CHECK(s.pending() == 0);
    CHECK_FALSE(s.cancel(other));
}

TEST_CASE("scheduler: run_immediately and exceptions") {
    forge::Scheduler s;
    std::atomic<int> n{0};
    s.after(1ms, [] { throw std::runtime_error("ignored"); });
    s.every(1h, [&] { ++n; }, true);
    CHECK(forge::testing::wait_until([&] { return n == 1; }));
    s.stop();
    CHECK(s.pending() == 0);
}

TEST_CASE("scheduler: periodic task keeps mutable state between ticks") {
    forge::Scheduler s;
    std::atomic<int> last{0};
    s.every(1ms, [&last, i = 0]() mutable { last = ++i; });
    CHECK(forge::testing::wait_until([&] { return last >= 3; }));
}

TEST_CASE("scheduler: manual clock runs timers without waiting") {
    using namespace std::chrono_literals;
    forge::Scheduler s(forge::Scheduler::manual_clock);
    CHECK(s.manual());
    std::vector<std::string> order;
    int ticks = 0;
    s.every(1s, [&] { ++ticks; });
    s.after(2500ms, [&] {
        order.push_back("once");
        s.after(1s, [&] { order.push_back("chained"); });  // scheduled from a task, at its due time
    });
    const auto t0 = s.now();
    CHECK(s.advance(999ms) == 0);
    CHECK(ticks == 0);
    CHECK(s.advance(1ms) == 1);
    CHECK(ticks == 1);
    s.advance(10s);  // every tick runs, in order; the chained task too
    CHECK(ticks == 11);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "once");
    CHECK(order[1] == "chained");
    CHECK(s.now() - t0 == 11s);
    CHECK(s.pending() == 1);  // the periodic one

    forge::Scheduler real;
    CHECK_THROWS_AS(real.advance(1s), std::logic_error);
}
