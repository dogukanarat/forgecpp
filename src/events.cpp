#include "forge/events.hpp"

#include "forge/log.hpp"

#include <exception>

namespace forge {

EventBus::EventBus() = default;

Subscription EventBus::add(std::type_index type, Handler handler) {
    std::shared_ptr<Handlers> list;
    {
        std::lock_guard lock(mutex_);
        auto& slot = handlers_[type];
        if (!slot) slot = std::make_shared<Handlers>();
        list = slot;
    }
    // An exception in one handler is logged; the others still run.
    return list->add([type, h = std::move(handler)](const void* const& event) {
        try {
            h(event);
        } catch (const std::exception& e) {
            log::error("event handler for {} threw: {}", type.name(), e.what());
        }
    });
}

void EventBus::dispatch(std::type_index type, const void* event) const {
    std::shared_ptr<Handlers> list;
    {
        std::lock_guard lock(mutex_);
        auto it = handlers_.find(type);
        if (it == handlers_.end()) return;
        list = it->second;
    }
    list->notify(event);
}

std::size_t EventBus::count(std::type_index type) const {
    std::lock_guard lock(mutex_);
    auto it = handlers_.find(type);
    return it == handlers_.end() ? 0 : it->second->size();
}

}  // namespace forge
