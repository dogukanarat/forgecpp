#include <forge/thread_pool.hpp>
#include <forge/testing.hpp>

#include <atomic>
#include <numeric>
#include <vector>

TEST_CASE("thread pool: submit, detach, loops") {
    forge::ThreadPool pool(4);
    CHECK(pool.get_thread_count() == 4);

    auto f = pool.submit_task([] { return 6 * 7; });
    CHECK(f.get() == 42);

    std::atomic<int> counter{0};
    for (int i = 0; i < 100; ++i) pool.detach_task([&] { ++counter; });
    pool.wait();
    CHECK(counter == 100);

    std::vector<int> v(1000);
    pool.detach_loop(std::size_t{0}, v.size(), [&](std::size_t i) { v[i] = static_cast<int>(i); });
    pool.wait();
    CHECK(std::accumulate(v.begin(), v.end(), 0LL) == 999LL * 1000 / 2);
}
