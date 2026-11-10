#pragma once
// Base64 (RFC 4648, with '=' padding): HTTP Basic auth, bytes fields in JSON.

#include <optional>
#include <string>
#include <string_view>

namespace forge {

std::string base64_encode(std::string_view bytes);
/// nullopt for a character outside the alphabet or a wrong length.
std::optional<std::string> base64_decode(std::string_view text);

}  // namespace forge
