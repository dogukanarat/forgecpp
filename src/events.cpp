#include "forge/events.hpp"

#include "forge/log.hpp"

namespace forge {

struct EventBus::Impl {
    mutable std::shared_mutex mutex;
    std::uint64_t next_id = 1;
    std::unordered_map<std::type_index, std::vector<std::pair<std::uint64_t, std::shared_ptr<Handler>>>> handlers;

    void remove(std::type_index type, std::uint64_t id) {
        std::unique_lock lock(mutex);
        auto it = handlers.find(type);
        if (it == handlers.end()) return;
        auto& vec = it->second;
        for (auto h = vec.begin(); h != vec.end(); ++h) {
            if (h->first == id) {
                vec.erase(h);
                break;
            }
        }
        if (vec.empty()) handlers.erase(it);
    }
};

EventBus::Subscription& EventBus::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        reset();
        impl_ = std::move(other.impl_);
        type_ = other.type_;
        id_ = std::exchange(other.id_, 0);
    }
    return *this;
}

void EventBus::Subscription::reset() {
    if (id_ == 0) return;
    if (auto impl = impl_.lock()) impl->remove(type_, id_);
    impl_.reset();
    id_ = 0;
}

EventBus::EventBus() : impl_(std::make_shared<Impl>()) {}

std::uint64_t EventBus::add(std::type_index type, std::shared_ptr<Handler> handler) {
    std::unique_lock lock(impl_->mutex);
    const auto id = impl_->next_id++;
    impl_->handlers[type].emplace_back(id, std::move(handler));
    return id;
}

void EventBus::dispatch(std::type_index type, const void* event) const {
    std::vector<std::shared_ptr<Handler>> snapshot;
    {
        std::shared_lock lock(impl_->mutex);
        auto it = impl_->handlers.find(type);
        if (it == impl_->handlers.end()) return;
        snapshot.reserve(it->second.size());
        for (const auto& [id, h] : it->second) snapshot.push_back(h);
    }
    for (const auto& h : snapshot) {
        try {
            (*h)(event);
        } catch (const std::exception& e) {
            log::error("event handler for {} threw: {}", type.name(), e.what());
        }
    }
}

std::size_t EventBus::count(std::type_index type) const {
    std::shared_lock lock(impl_->mutex);
    auto it = impl_->handlers.find(type);
    return it == impl_->handlers.end() ? 0 : it->second.size();
}

}  // namespace forge
