#include "forge/systemd.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace forge::systemd {

bool available() {
    const char* path = std::getenv("NOTIFY_SOCKET");
    return path != nullptr && path[0] != '\0';
}

bool notify(std::string_view state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || path[0] == '\0') return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::size_t len = std::strlen(path);
    if (len >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path, len);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';  // abstract namespace

    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const auto addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + len);
    const auto sent = ::sendto(fd, state.data(), state.size(), MSG_NOSIGNAL,
                               reinterpret_cast<const sockaddr*>(&addr), addr_len);
    ::close(fd);
    return sent == static_cast<ssize_t>(state.size());
}

std::optional<std::chrono::microseconds> watchdog_interval() {
    const char* usec = std::getenv("WATCHDOG_USEC");
    if (usec == nullptr) return std::nullopt;
    if (const char* pid = std::getenv("WATCHDOG_PID")) {
        if (std::to_string(::getpid()) != pid) return std::nullopt;
    }
    char* end = nullptr;
    const auto value = std::strtoull(usec, &end, 10);
    if (end == usec || value == 0) return std::nullopt;
    return std::chrono::microseconds(value);
}

}  // namespace forge::systemd
