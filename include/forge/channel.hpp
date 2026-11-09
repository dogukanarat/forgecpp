#pragma once
// Lock-free multi-producer / multi-consumer channel built on
// moodycamel::BlockingConcurrentQueue.
//
//   forge::Channel<Sample> samples;
//   samples.push(Sample{...});                       // any thread
//   while (auto s = samples.pop()) handle(*s);      // blocks until item or close()

#include <moodycamel/blockingconcurrentqueue.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <optional>
#include <utility>

namespace forge {

template <class T>
class Channel {
public:
    /// How often blocking pops re-check the closed flag.
    static constexpr std::chrono::milliseconds close_poll_interval{50};

    explicit Channel(std::size_t initial_capacity = 64) : queue_(initial_capacity) {}

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    /// Enqueue an item. Returns false if the channel is closed or allocation failed.
    bool push(T value) {
        if (closed_.load(std::memory_order_acquire)) return false;
        return queue_.enqueue(std::move(value));
    }

    template <class... Args>
    bool emplace(Args&&... args) {
        return push(T(std::forward<Args>(args)...));
    }

    /// Bulk enqueue from an iterator range (moves the items).
    template <class It>
    bool push_bulk(It first, std::size_t count) {
        if (closed_.load(std::memory_order_acquire)) return false;
        return queue_.enqueue_bulk(std::make_move_iterator(first), count);
    }

    /// Non-blocking pop.
    [[nodiscard]] std::optional<T> try_pop() {
        T item;
        if (queue_.try_dequeue(item)) return std::optional<T>(std::move(item));
        return std::nullopt;
    }

    /// Wait at most `timeout` for an item.
    template <class Rep, class Period>
    [[nodiscard]] std::optional<T> pop_for(std::chrono::duration<Rep, Period> timeout) {
        T item;
        if (queue_.wait_dequeue_timed(item, timeout)) return std::optional<T>(std::move(item));
        return std::nullopt;
    }

    /// Block until an item is available. Returns std::nullopt once the
    /// channel is closed *and* drained.
    [[nodiscard]] std::optional<T> pop() {
        T item;
        for (;;) {
            if (queue_.wait_dequeue_timed(item, close_poll_interval)) {
                return std::optional<T>(std::move(item));
            }
            if (closed_.load(std::memory_order_acquire)) {
                if (queue_.try_dequeue(item)) return std::optional<T>(std::move(item));
                return std::nullopt;
            }
        }
    }

    /// Dequeue up to `max` items into `out`, waiting at most `timeout` for the first one.
    template <class OutIt, class Rep, class Period>
    std::size_t pop_bulk(OutIt out, std::size_t max, std::chrono::duration<Rep, Period> timeout) {
        return queue_.wait_dequeue_bulk_timed(out, max, timeout);
    }

    /// Approximate number of queued items.
    std::size_t size_approx() const { return queue_.size_approx(); }

    /// Reject further pushes; consumers drain the remaining items and then stop.
    void close() { closed_.store(true, std::memory_order_release); }
    bool closed() const { return closed_.load(std::memory_order_acquire); }

    moodycamel::BlockingConcurrentQueue<T>& native() { return queue_; }

private:
    moodycamel::BlockingConcurrentQueue<T> queue_;
    std::atomic<bool> closed_{false};
};

}  // namespace forge
