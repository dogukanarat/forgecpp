#include "forge/status.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace forge {
namespace {

struct ErrnoName {
    const char* name;
    std::uint16_t wire;  // Zephyr / newlib / picolibc number
    int host;
};

// Every errno Zephyr's libc defines (lib/libc/minimal/include/errno.h), with
// the host's number where the host has the same name. The first entry for a
// host number wins host -> wire (Linux's ENOTSUP and EOPNOTSUPP are both 95;
// Zephyr code returns ENOTSUP).
#define FORGE_ERRNO(name, wire) {#name, wire, name},
// clang-format off
const ErrnoName kErrnos[] = {
    FORGE_ERRNO(EPERM, 1)          FORGE_ERRNO(ENOENT, 2)         FORGE_ERRNO(ESRCH, 3)
    FORGE_ERRNO(EINTR, 4)          FORGE_ERRNO(EIO, 5)            FORGE_ERRNO(ENXIO, 6)
    FORGE_ERRNO(E2BIG, 7)          FORGE_ERRNO(ENOEXEC, 8)        FORGE_ERRNO(EBADF, 9)
    FORGE_ERRNO(ECHILD, 10)        FORGE_ERRNO(EAGAIN, 11)        FORGE_ERRNO(ENOMEM, 12)
    FORGE_ERRNO(EACCES, 13)        FORGE_ERRNO(EFAULT, 14)        FORGE_ERRNO(ENOTBLK, 15)
    FORGE_ERRNO(EBUSY, 16)         FORGE_ERRNO(EEXIST, 17)        FORGE_ERRNO(EXDEV, 18)
    FORGE_ERRNO(ENODEV, 19)        FORGE_ERRNO(ENOTDIR, 20)       FORGE_ERRNO(EISDIR, 21)
    FORGE_ERRNO(EINVAL, 22)        FORGE_ERRNO(ENFILE, 23)        FORGE_ERRNO(EMFILE, 24)
    FORGE_ERRNO(ENOTTY, 25)        FORGE_ERRNO(ETXTBSY, 26)       FORGE_ERRNO(EFBIG, 27)
    FORGE_ERRNO(ENOSPC, 28)        FORGE_ERRNO(ESPIPE, 29)        FORGE_ERRNO(EROFS, 30)
    FORGE_ERRNO(EMLINK, 31)        FORGE_ERRNO(EPIPE, 32)         FORGE_ERRNO(EDOM, 33)
    FORGE_ERRNO(ERANGE, 34)        FORGE_ERRNO(ENOMSG, 35)        FORGE_ERRNO(EDEADLK, 45)
    FORGE_ERRNO(ENOLCK, 46)        FORGE_ERRNO(ENOSTR, 60)        FORGE_ERRNO(ENODATA, 61)
    FORGE_ERRNO(ETIME, 62)         FORGE_ERRNO(ENOSR, 63)         FORGE_ERRNO(EPROTO, 71)
    FORGE_ERRNO(EBADMSG, 77)       FORGE_ERRNO(ENOSYS, 88)        FORGE_ERRNO(ENOTEMPTY, 90)
    FORGE_ERRNO(ENAMETOOLONG, 91)  FORGE_ERRNO(ELOOP, 92)         FORGE_ERRNO(ENOTSUP, 134)
    FORGE_ERRNO(EOPNOTSUPP, 95)    FORGE_ERRNO(EPFNOSUPPORT, 96)  FORGE_ERRNO(ECONNRESET, 104)
    FORGE_ERRNO(ENOBUFS, 105)      FORGE_ERRNO(EAFNOSUPPORT, 106) FORGE_ERRNO(EPROTOTYPE, 107)
    FORGE_ERRNO(ENOTSOCK, 108)     FORGE_ERRNO(ENOPROTOOPT, 109)  FORGE_ERRNO(ESHUTDOWN, 110)
    FORGE_ERRNO(ECONNREFUSED, 111) FORGE_ERRNO(EADDRINUSE, 112)   FORGE_ERRNO(ECONNABORTED, 113)
    FORGE_ERRNO(ENETUNREACH, 114)  FORGE_ERRNO(ENETDOWN, 115)     FORGE_ERRNO(ETIMEDOUT, 116)
    FORGE_ERRNO(EHOSTDOWN, 117)    FORGE_ERRNO(EHOSTUNREACH, 118) FORGE_ERRNO(EINPROGRESS, 119)
    FORGE_ERRNO(EALREADY, 120)     FORGE_ERRNO(EDESTADDRREQ, 121) FORGE_ERRNO(EMSGSIZE, 122)
    FORGE_ERRNO(EPROTONOSUPPORT, 123) FORGE_ERRNO(ESOCKTNOSUPPORT, 124) FORGE_ERRNO(EADDRNOTAVAIL, 125)
    FORGE_ERRNO(ENETRESET, 126)    FORGE_ERRNO(EISCONN, 127)      FORGE_ERRNO(ENOTCONN, 128)
    FORGE_ERRNO(ETOOMANYREFS, 129) FORGE_ERRNO(EILSEQ, 138)       FORGE_ERRNO(EOVERFLOW, 139)
    FORGE_ERRNO(ECANCELED, 140)
};
// clang-format on
#undef FORGE_ERRNO

constexpr std::uint16_t kWireEio = 5;

const ErrnoName* by_wire(std::uint16_t wire) {
    for (const auto& e : kErrnos) {
        if (e.wire == wire) return &e;
    }
    return nullptr;
}

std::mutex& names_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::uint16_t, std::string>& names() {
    static std::map<std::uint16_t, std::string> n{
        {module_id::app, "app"}, {module_id::c2, "c2"},   {module_id::tmtc, "tmtc"},    {module_id::pb, "pb"},
        {module_id::zmq, "zmq"}, {module_id::nrf, "nrf"}, {module_id::zephyr, "zephyr"}};
    return n;
}

}  // namespace

std::uint16_t errno_to_wire(int host_errno) {
    if (host_errno < 0) host_errno = -host_errno;
    if (host_errno == 0) return 0;
    for (const auto& e : kErrnos) {
        if (e.host == host_errno) return e.wire;
    }
    return kWireEio;
}

int errno_from_wire(std::uint16_t wire_code) {
    if (wire_code == 0) return 0;
    const auto* e = by_wire(wire_code);
    return e ? e->host : EIO;
}

Status Status::error(std::uint16_t module, int errno_code, const char* message) {
    return wire_error(module, errno_to_wire(errno_code == 0 ? EIO : errno_code), message);
}

int Status::errno_code() const {
    if (ok()) return 0;
    if (module() == module_id::nrf) return EIO;
    return errno_from_wire(wire_code());
}

std::string Status::to_string() const {
    if (ok()) return "ok";
    std::string origin;
    const auto id = module();
    if (id != module_id::unknown) {
        std::lock_guard lock(names_mutex());
        const auto it = names().find(id);
        if (it != names().end()) {
            origin = it->second;
        } else {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "module 0x%04x", id);
            origin = buf;
        }
    }
    std::string reason;
    if (id == module_id::nrf) {
        reason = message_ ? message_ : "modem error";
        reason += " (code " + std::to_string(wire_code()) + ")";
    } else if (const auto* e = by_wire(wire_code())) {
        reason = message_ ? message_ : std::strerror(e->host);
        reason += std::string(" (") + e->name + ")";
    } else {
        reason = message_ ? message_ : "error";
        reason += " (code " + std::to_string(wire_code()) + ")";
    }
    return origin.empty() ? reason : origin + ": " + reason;
}

int Status::http_status() const {
    if (ok()) return 200;
    if (module() == module_id::nrf) return 502;  // the modem refused: a gateway's answer
    switch (wire_code()) {
        case 22:   // EINVAL
        case 34:   // ERANGE
        case 77:   // EBADMSG
        case 122:  // EMSGSIZE
        case 138:  // EILSEQ
        case 139:  // EOVERFLOW
        case 7:    // E2BIG
            return 400;
        case 1:   // EPERM
        case 13:  // EACCES
            return 403;
        case 2:   // ENOENT
        case 6:   // ENXIO
        case 19:  // ENODEV
            return 404;
        case 16:   // EBUSY
        case 17:   // EEXIST
        case 120:  // EALREADY
        case 119:  // EINPROGRESS
            return 409;
        case 88:   // ENOSYS
        case 95:   // EOPNOTSUPP
        case 134:  // ENOTSUP
        case 123:  // EPROTONOSUPPORT
            return 501;
        case 11:   // EAGAIN
        case 12:   // ENOMEM
        case 105:  // ENOBUFS
        case 28:   // ENOSPC
        case 128:  // ENOTCONN
        case 115:  // ENETDOWN
        case 118:  // EHOSTUNREACH
            return 503;
        case 116:  // ETIMEDOUT
        case 62:   // ETIME
            return 504;
        default: return 500;
    }
}

int errno_for_http(int http_status) {
    switch (http_status) {
        case 400: return EINVAL;
        case 401:
        case 403: return EACCES;
        case 404: return ENOENT;
        case 409: return EBUSY;
        case 410:
        case 501: return ENOSYS;
        case 503: return EAGAIN;
        case 504: return ETIMEDOUT;
        default: return http_status >= 200 && http_status < 300 ? 0 : EIO;
    }
}

void name_module(std::uint16_t id, std::string_view name) {
    std::lock_guard lock(names_mutex());
    names()[id] = std::string(name);
}

}  // namespace forge
