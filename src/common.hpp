#pragma once
// Small helpers several parts of the library share (not installed).
#include <string>
#include <string_view>

namespace forge::detail {

/// '\n' -> "\r\n" (a raw terminal, telnet).
std::string crlf(std::string_view text);

/// Equal, in time that does not depend on where they differ (secrets).
bool constant_time_equal(std::string_view a, std::string_view b);

/// The machine's host name, read once.
const std::string& host_name();

}  // namespace forge::detail
