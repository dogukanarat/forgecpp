#include "signals.hpp"

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cstdint>

namespace forge::detail {
namespace {

sigset_t handled() {
    sigset_t set;
    sigemptyset(&set);
    for (int sig : {SIGINT, SIGTERM, SIGHUP, SIGQUIT}) sigaddset(&set, sig);
    return set;
}

}  // namespace

SignalWatcher::~SignalWatcher() { stop(); }

void SignalWatcher::block() {
    const sigset_t set = handled();
    pthread_sigmask(SIG_BLOCK, &set, &saved_mask_);
    blocked_ = true;
}

void SignalWatcher::start(std::function<void(int)> on_signal) {
    if (thread_.joinable()) return;
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    thread_ = std::thread([this, on_signal = std::move(on_signal)] {
        const sigset_t set = handled();
        const int sfd = ::signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
        pollfd fds[] = {{sfd, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
        for (;;) {
            if (::poll(fds, 2, -1) <= 0) continue;  // EINTR
            if (fds[1].revents & POLLIN) break;     // stop()
            signalfd_siginfo info{};
            if (::read(sfd, &info, sizeof(info)) == static_cast<ssize_t>(sizeof(info))) {
                on_signal(static_cast<int>(info.ssi_signo));
            }
        }
        ::close(sfd);
    });
}

void SignalWatcher::stop() {
    if (!thread_.joinable()) return;
    const std::uint64_t one = 1;
    (void)!::write(wake_fd_, &one, sizeof(one));
    thread_.join();
    ::close(wake_fd_);
    wake_fd_ = -1;
}

void SignalWatcher::restore() {
    if (!blocked_) return;
    pthread_sigmask(SIG_SETMASK, &saved_mask_, nullptr);
    blocked_ = false;
}

}  // namespace forge::detail
