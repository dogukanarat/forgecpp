#include "forge/worker.hpp"

#include "forge/log.hpp"
#include "forge/trace.hpp"

namespace forge {

Worker::Worker(std::string name, Body body, OnError on_error) : name_(std::move(name)) {
    thread_ = std::jthread([this, body = std::move(body), on_error = std::move(on_error)](std::stop_token stop) {
        trace::set_thread_name(name_);
        try {
            body(stop);
        } catch (const std::exception& e) {
            log::error("worker {} ended: {}", name_, e.what());
            if (on_error) on_error(e.what());
        } catch (...) {
            log::error("worker {} ended: unknown exception", name_);
            if (on_error) on_error("unknown exception");
        }
        running_ = false;
    });
}

Worker::~Worker() { stop(); }

void Worker::stop() {
    if (!thread_.joinable()) return;
    thread_.request_stop();
    thread_.join();
}

bool Worker::running() const { return running_; }

}  // namespace forge
