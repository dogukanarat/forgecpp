#include "forge/base64.hpp"

namespace forge {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value_of(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(std::string_view in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out += {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63], kAlphabet[v & 63]};
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += {kAlphabet[v >> 18], kAlphabet[(v >> 12) & 63], kAlphabet[(v >> 6) & 63], '='};
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view in) {
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    if (in.size() % 4 == 1) return std::nullopt;
    std::string out;
    out.reserve(in.size() * 3 / 4);
    unsigned acc = 0;
    int bits = 0;
    for (char c : in) {
        const int v = value_of(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace forge
