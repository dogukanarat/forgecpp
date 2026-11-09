#pragma once
// Recycled buffers for hot paths: no allocation per message once warm.
//
//   auto pool = forge::BufferPool<std::int16_t>::create(2 * 65536, 16);  // 16 buffers of 128 k int16
//   auto buf = pool->acquire();           // shared_ptr<std::vector<int16_t>>, sized
//   fill(buf->data());
//   // zero copy: the stream holds `buf` until sent, then it returns to the pool
//   stream.publish(msg, buf, buf->data(), buf->size() * sizeof(std::int16_t));
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
        // The handle's control block is recycled too (NodeAllocator): once
        // warm, acquire() allocates nothing at all.
        return Handle(b, [weak](Buffer* p) {
            if (auto pool = weak.lock()) {
                pool->release(p);
            } else {
                delete p;
            }
        }, NodeAllocator<Buffer>(nodes_));
    }

    std::size_t buffer_size() const { return size_; }
    /// Buffers allocated so far (a steady value means no allocations in the hot path).
    std::size_t allocations() const { return allocations_.load(); }
    std::size_t free_buffers() const { return free_count_.load(); }

private:
    /// Fixed-size blocks for the handles' control blocks, kept on a lock-free list.
    struct Nodes {
        moodycamel::ConcurrentQueue<void*> free;
        std::atomic<std::size_t> block_size{0};  // the one size this pool's control blocks have
        ~Nodes() {
            void* p = nullptr;
            while (free.try_dequeue(p)) ::operator delete(p);
        }
    };
    template <class U>
    struct NodeAllocator {
        using value_type = U;
        std::shared_ptr<Nodes> nodes;
        explicit NodeAllocator(std::shared_ptr<Nodes> n) : nodes(std::move(n)) {}
        template <class V>
        NodeAllocator(const NodeAllocator<V>& other) : nodes(other.nodes) {}
        U* allocate(std::size_t n) {
            const std::size_t bytes = n * sizeof(U);
            std::size_t expected = 0;
            nodes->block_size.compare_exchange_strong(expected, bytes);
            void* p = nullptr;
            if (bytes == nodes->block_size.load() && nodes->free.try_dequeue(p)) return static_cast<U*>(p);
            return static_cast<U*>(::operator new(bytes));
        }
        void deallocate(U* p, std::size_t n) {
            if (n * sizeof(U) == nodes->block_size.load() && nodes->free.size_approx() < 256) {
                nodes->free.enqueue(p);
            } else {
                ::operator delete(p);
            }
        }
        template <class V>
        bool operator==(const NodeAllocator<V>& o) const {
            return nodes == o.nodes;
        }
    };

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
    std::shared_ptr<Nodes> nodes_ = std::make_shared<Nodes>();
    std::atomic<std::size_t> allocations_{0};
};

}  // namespace forge
