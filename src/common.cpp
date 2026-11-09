#include "common.hpp"

#include <unistd.h>

namespace forge::detail {

std::string crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

bool constant_time_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

const std::string& host_name() {
    static const std::string host = [] {
        char buf[256] = {};
        ::gethostname(buf, sizeof(buf) - 1);
        return std::string(buf);
    }();
    return host;
}

}  // namespace forge::detail
