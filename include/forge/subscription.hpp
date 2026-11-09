#pragma once
// Callbacks with an owner: add() returns a Subscription, and the callback is
// gone when the Subscription is (also while it runs on another thread: the
// destructor waits for it). The list itself may go first; nothing dangles.
//
//   forge::Listeners<Report> changed_;                 // in the class that tells
//   changed_.notify(report);
//
//   sub_ = health.on_change([this](const Health::Report& r) { ... });  // in the one that listens

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace forge {

class Subscription {
public:
    Subscription() = default;
    explicit Subscription(std::function<void()> cancel) : cancel_(std::move(cancel)) {}
    Subscription(Subscription&& other) noexcept : cancel_(std::exchange(other.cancel_, nullptr)) {}
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            reset();
            cancel_ = std::exchange(other.cancel_, nullptr);
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription() { reset(); }

    /// Remove the callback now. Returns once it is not running.
    void reset() {
        if (auto cancel = std::exchange(cancel_, nullptr)) cancel();
    }
    bool active() const { return static_cast<bool>(cancel_); }

private:
    std::function<void()> cancel_;
};

template <class... Args>
class Listeners {
public:
    using Callback = std::function<void(const Args&...)>;

    [[nodiscard]] Subscription add(Callback callback) {
        std::uint64_t id = 0;
        {
            std::lock_guard lock(impl_->mutex);
            id = ++impl_->next;
            impl_->callbacks.emplace(id, std::move(callback));
        }
        return Subscription([weak = std::weak_ptr<Impl>(impl_), id] {
            if (auto impl = weak.lock()) {
                {
                    std::lock_guard lock(impl->mutex);
                    impl->callbacks.erase(id);
                }
                std::lock_guard wait(impl->calling);  // recursive: a callback may remove itself
            }
        });
    }

    /// Call every callback, on this thread. One notify() at a time.
    void notify(const Args&... args) const {
        std::lock_guard calling(impl_->calling);
        std::map<std::uint64_t, Callback> now;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->callbacks.empty()) return;
            now = impl_->callbacks;
        }
        for (const auto& [id, callback] : now) {
            {
                std::lock_guard lock(impl_->mutex);
                if (!impl_->callbacks.contains(id)) continue;  // removed by an earlier one
            }
            callback(args...);
        }
    }

    bool empty() const {
        std::lock_guard lock(impl_->mutex);
        return impl_->callbacks.empty();
    }
    std::size_t size() const {
        std::lock_guard lock(impl_->mutex);
        return impl_->callbacks.size();
    }

private:
    struct Impl {
        std::mutex mutex;
        std::recursive_mutex calling;
        std::map<std::uint64_t, Callback> callbacks;
        std::uint64_t next = 0;
    };
    std::shared_ptr<Impl> impl_ = std::make_shared<Impl>();
};

}  // namespace forge
