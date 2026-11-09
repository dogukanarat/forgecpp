#pragma once
// forge::Status: the result of a command, in the form the c2 protocol carries it.
//
// It is the int32_t status of zephyrus (common_error.h), so that a device
// running forgecpp and a microcontroller running zephyrus read each other's
// errors the same way:
//
//    31   30 .............. 16   15 ................ 0
//   +-----+--------------------+--------------------+
//   |  1  |     MODULE ID      |     ERROR CODE     |
//   +-----+--------------------+--------------------+
//
// Zero is success. An error always has bit 31 set (it is negative as int32_t).
// The module id says where the error comes from, the code says why.
//
// Error codes are errno values. Their numbers are not the same on every C
// library (ENOSYS is 38 on Linux and 88 on Zephyr), so on the wire they always
// use Zephyr's numbers (those of newlib and picolibc). Status converts: write
// the host's names (EINVAL, ENOSYS, ...) and read them back with errno().
//
//   if (hz > max) return forge::Status::error(forge::module_id::app, ERANGE, "frequency too high");
//   std::int32_t on_the_wire = status.wire();
//   auto s = forge::Status::from_wire(on_the_wire);   // s.errno_code() == ERANGE
//
// The message is for logs and the web: it does not travel in a binary reply
// (a NACK carries the four bytes of wire() only). It must be a string literal
// or otherwise outlive the Status: it is not copied.

#include <cstdint>
#include <string>
#include <string_view>

namespace forge {

/// Module ids (15 bits). The reserved ones and the application's are those of zephyrus.
namespace module_id {
inline constexpr std::uint16_t unknown = 0;          ///< no origin: a plain errno
inline constexpr std::uint16_t app = 64;             ///< application code (zephyrus MODULE_ID_APP)
inline constexpr std::uint16_t external_base = 128;  ///< first id for modules outside zephyrus
inline constexpr std::uint16_t forge_base = 0x0100;  ///< forgecpp's own modules
inline constexpr std::uint16_t c2 = forge_base + 0;
inline constexpr std::uint16_t tmtc = forge_base + 1;
inline constexpr std::uint16_t pb = forge_base + 2;
inline constexpr std::uint16_t zmq = forge_base + 3;
inline constexpr std::uint16_t nrf = 0x7FFD;         ///< Nordic modem library: codes are not errnos
inline constexpr std::uint16_t zephyr = 0x7FFE;      ///< Zephyr kernel, drivers, libc
inline constexpr std::uint16_t raw = 0x7FFF;         ///< reserved: a sign-extended plain errno
inline constexpr std::uint16_t max = nrf - 1;        ///< largest id that can be assigned
}  // namespace module_id

class Status {
public:
    /// Success.
    constexpr Status() = default;

    /// An error of `module` (0 .. module_id::max) with a host errno code
    /// (EINVAL and -EINVAL are the same). `message`: see the file comment.
    static Status error(std::uint16_t module, int errno_code, const char* message = nullptr);
    /// An error whose code is already in wire (Zephyr) numbering, or is not an
    /// errno at all (module_id::nrf).
    static constexpr Status wire_error(std::uint16_t module, std::uint16_t wire_code, const char* message = nullptr) {
        Status s;
        s.value_ = static_cast<std::int32_t>(0x80000000U | ((static_cast<std::uint32_t>(module) << 16) & 0x7FFF0000U) |
                                             wire_code);
        s.message_ = message;
        return s;
    }
    /// The int32_t of zephyrus, as a function returns it or a NACK carries it.
    /// A plain negative errno (from code that does not tag its errors) is read
    /// as module_id::unknown; zero and positive values are success.
    static constexpr Status from_wire(std::int32_t value) {
        Status s;
        s.value_ = value;
        return s;
    }

    constexpr bool ok() const { return value_ >= 0; }
    /// The value on the wire (0 for success).
    constexpr std::int32_t wire() const { return ok() ? 0 : value_; }
    /// The origin; module_id::unknown for success and for plain errnos.
    constexpr std::uint16_t module() const {
        if (ok() || is_raw()) return module_id::unknown;
        return static_cast<std::uint16_t>((static_cast<std::uint32_t>(value_) & 0x7FFF0000U) >> 16);
    }
    /// The code in wire (Zephyr) numbering; 0 for success.
    constexpr std::uint16_t wire_code() const {
        if (ok()) return 0;
        if (is_raw()) {
            const std::int64_t magnitude = -static_cast<std::int64_t>(value_);
            return magnitude <= 0xFFFF ? static_cast<std::uint16_t>(magnitude) : 0;
        }
        return static_cast<std::uint16_t>(static_cast<std::uint32_t>(value_) & 0xFFFFU);
    }
    /// The code as a host errno (EINVAL, ENOSYS, ...); 0 for success; EIO for a
    /// code this host has no errno for, and for module_id::nrf (not an errno).
    int errno_code() const;
    /// Message given when the error was made, or nullptr.
    const char* message() const { return message_; }

    /// "c2: Message too long (EMSGSIZE)", "module 0x0042: ...", "ok".
    std::string to_string() const;
    /// The HTTP status that answers a web request with this result.
    int http_status() const;

    /// Same result (module and code); the message does not count.
    friend constexpr bool operator==(const Status& a, const Status& b) { return a.wire() == b.wire(); }

private:
    constexpr bool is_raw() const {
        return ((static_cast<std::uint32_t>(value_) & 0x7FFF0000U) >> 16) == module_id::raw;
    }

    std::int32_t value_ = 0;
    const char* message_ = nullptr;
};

/// The name shown for a module id in Status::to_string() (forgecpp's own are
/// known already). Call it once at start-up for each module of yours.
void name_module(std::uint16_t id, std::string_view name);

/// The errno that stands for an HTTP status (400 -> EINVAL, 404 -> ENOENT, ...).
int errno_for_http(int http_status);

/// Host errno <-> wire (Zephyr) errno. Unknown values give EIO's number.
std::uint16_t errno_to_wire(int host_errno);
int errno_from_wire(std::uint16_t wire_code);

}  // namespace forge
