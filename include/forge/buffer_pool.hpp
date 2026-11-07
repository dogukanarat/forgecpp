#pragma once
// Recycled buffers for hot paths: no allocation per message once warm.
//
//   forge::BufferPool<std::int16_t> pool(2 * 65536, 16);   // 16 buffers of 128 k int16
//   auto buf = pool.acquire();            // shared_ptr<std::vector<int16_t>>, sized
//   fill(buf->data());
//   stream.publish(msg, buf);             // returns to the pool when the last user drops it
//
// acquire() never blocks: when every buffer is in use it allocates a new one
// (counted in allocations()) and the pool keeps up to `max_free` for later.

#include <moodycamel/concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

namespace forge {

template <class T>
class BufferPool : public std::enable_shared_from_this<BufferPool<T>> {
    struct Private {};

public:
    using Buffer = std::vector<T>;
    using Handle = std::shared_ptr<Buffer>;

    static std::shared_ptr<BufferPool> create(std::size_t buffer_size, std::size_t preallocate = 4,
                                              std::size_t max_free = 64) {
        auto p = std::make_shared<BufferPool>(Private{}, buffer_size, max_free);
        for (std::size_t i = 0; i < preallocate; ++i) p->free_.enqueue(new Buffer(buffer_size));
        p->free_count_ = preallocate;
        p->allocations_ = preallocate;
        return p;
    }

    BufferPool(Private, std::size_t buffer_size, std::size_t max_free) : size_(buffer_size), max_free_(max_free) {}

    ~BufferPool() {
        Buffer* b = nullptr;
        while (free_.try_dequeue(b)) delete b;
    }

    /// A buffer of buffer_size() elements (contents unspecified).
    Handle acquire() {
        Buffer* b = nullptr;
        if (free_.try_dequeue(b)) {
            --free_count_;
        } else {
            b = new Buffer(size_);
            ++allocations_;
        }
        if (b->size() != size_) b->resize(size_);
        std::weak_ptr<BufferPool> weak = this->shared_from_this();
        return Handle(b, [weak](Buffer* p) {
            if (auto pool = weak.lock()) {
                pool->release(p);
            } else {
                delete p;
            }
        });
    }

    std::size_t buffer_size() const { return size_; }
    /// Buffers allocated so far (a steady value means no allocations in the hot path).
    std::size_t allocations() const { return allocations_.load(); }
    std::size_t free_buffers() const { return free_count_.load(); }

private:
    void release(Buffer* p) {
        if (free_count_.load() >= max_free_) {
            delete p;
            return;
        }
        ++free_count_;
        free_.enqueue(p);
    }

    std::size_t size_;
    std::size_t max_free_;
    moodycamel::ConcurrentQueue<Buffer*> free_;
    std::atomic<std::size_t> free_count_{0};
    std::atomic<std::size_t> allocations_{0};
};

}  // namespace forge
