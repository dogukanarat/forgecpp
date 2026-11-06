#include <forge/channel.hpp>
#include <forge/testing.hpp>

#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("channel: basic push/pop") {
    forge::Channel<int> ch;
    CHECK_FALSE(ch.try_pop().has_value());
    CHECK(ch.push(1));
    CHECK(ch.emplace(2));
    CHECK(ch.try_pop().value() == 1);
    CHECK(ch.pop_for(std::chrono::milliseconds(10)).value() == 2);
    CHECK_FALSE(ch.pop_for(std::chrono::milliseconds(10)).has_value());
}

TEST_CASE("channel: close drains then stops consumers") {
    forge::Channel<int> ch;
    ch.push(1);
    ch.push(2);
    ch.close();
    CHECK_FALSE(ch.push(3));
    CHECK(ch.pop().value() == 1);
    CHECK(ch.pop().value() == 2);
    CHECK_FALSE(ch.pop().has_value());
}

TEST_CASE("channel: many producers, many consumers") {
    forge::Channel<int> ch;
    constexpr int producers = 4, per_producer = 10000;
    std::atomic<long long> sum{0};
    std::atomic<int> received{0};

    std::vector<std::thread> consumers;
    for (int i = 0; i < 3; ++i) {
        consumers.emplace_back([&] {
            while (auto v = ch.pop()) {
                sum += *v;
                ++received;
            }
        });
    }
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&] {
            for (int i = 1; i <= per_producer; ++i) ch.push(i);
        });
    }
    for (auto& t : threads) t.join();
    ch.close();
    for (auto& t : consumers) t.join();

    CHECK(received == producers * per_producer);
    CHECK(sum == static_cast<long long>(producers) * per_producer * (per_producer + 1) / 2);
}

TEST_CASE("channel: bulk") {
    forge::Channel<int> ch;
    std::vector<int> in{1, 2, 3, 4, 5};
    CHECK(ch.push_bulk(in.begin(), in.size()));
    std::vector<int> out(10);
    const auto n = ch.pop_bulk(out.begin(), out.size(), std::chrono::milliseconds(10));
    CHECK(n == 5);
}
