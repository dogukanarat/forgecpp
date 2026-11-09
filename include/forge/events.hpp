#pragma once
// Typed publish/subscribe event bus.
//
//   struct TemperatureChanged { double celsius; };
//   auto sub = bus.subscribe<TemperatureChanged>([](const TemperatureChanged& e) { ... });
//   bus.publish(TemperatureChanged{21.5});   // synchronous, in the caller's thread
//
// Subscriptions are RAII handles: the handler is removed when the handle is
// destroyed (or reset()). Keep it as a member of the subscribing object.

#include "forge/subscription.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace forge {

class EventBus {
public:
    /// The same RAII handle as every other listener in forgecpp: reset() (or
    /// the destructor) returns once the handler is not running anywhere.
    using Subscription = forge::Subscription;

    EventBus();

    template <class Event, class Fn>
    [[nodiscard]] Subscription subscribe(Fn&& fn) {
        return add(std::type_index(typeid(Event)),
                   [f = std::forward<Fn>(fn)](const void* e) { f(*static_cast<const Event*>(e)); });
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
    using Handlers = Listeners<const void*>;

    Subscription add(std::type_index type, Handler handler);
    void dispatch(std::type_index type, const void* event) const;
    std::size_t count(std::type_index type) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::shared_ptr<Handlers>> handlers_;
};

}  // namespace forge
