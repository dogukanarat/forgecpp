#pragma once
// Typed publish/subscribe event bus.
//
//   struct TemperatureChanged { double celsius; };
//   auto sub = bus.subscribe<TemperatureChanged>([](const TemperatureChanged& e) { ... });
//   bus.publish(TemperatureChanged{21.5});   // synchronous, in the caller's thread
//
// Subscriptions are RAII handles: the handler is removed when the handle is
// destroyed (or reset()). Keep it as a member of the subscribing object.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace forge {

class EventBus {
    struct Impl;

public:
    class Subscription {
    public:
        Subscription() = default;
        Subscription(Subscription&& other) noexcept { *this = std::move(other); }
        Subscription& operator=(Subscription&& other) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;
        ~Subscription() { reset(); }

        void reset();
        bool active() const { return !impl_.expired() && id_ != 0; }

    private:
        friend class EventBus;
        Subscription(std::weak_ptr<Impl> impl, std::type_index type, std::uint64_t id)
            : impl_(std::move(impl)), type_(type), id_(id) {}

        std::weak_ptr<Impl> impl_;
        std::type_index type_{typeid(void)};
        std::uint64_t id_ = 0;
    };

    EventBus();

    template <class Event, class Fn>
    [[nodiscard]] Subscription subscribe(Fn&& fn) {
        auto handler =
            std::make_shared<Handler>([f = std::forward<Fn>(fn)](const void* e) { f(*static_cast<const Event*>(e)); });
        const auto type = std::type_index(typeid(Event));
        return Subscription(impl_, type, add(type, std::move(handler)));
    }

    /// Deliver `event` to every subscriber, synchronously in the calling thread.
    /// Handlers may subscribe/unsubscribe while being called.
    template <class Event>
    void publish(const Event& event) const {
        dispatch(std::type_index(typeid(Event)), &event);
    }

    template <class Event>
    std::size_t subscriber_count() const {
        return count(std::type_index(typeid(Event)));
    }

private:
    using Handler = std::function<void(const void*)>;

    std::uint64_t add(std::type_index type, std::shared_ptr<Handler> handler);
    void dispatch(std::type_index type, const void* event) const;
    std::size_t count(std::type_index type) const;

    std::shared_ptr<Impl> impl_;
};

}  // namespace forge
