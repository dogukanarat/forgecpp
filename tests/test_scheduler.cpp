#include <forge/scheduler.hpp>
#include <forge/testing.hpp>

#include <atomic>

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
