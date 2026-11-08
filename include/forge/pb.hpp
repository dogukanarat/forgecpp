#pragma once
// Messages on the wire: nanopb structs with a bound on every field, so that
// encoding and decoding never allocate. forge-proto.py adds what nanopb does
// not keep: the full name of each message and the names of its fields, used
// for JSON (the web) and for type checks.
//
//   radio_tmtc_status_reply rep{};
//   forge::pb::assign(rep.state, "streaming");
//   std::array<std::uint8_t, 64> buf;
//   std::size_t n = 0;
//   forge::pb::encode(rep, buf, n);
//   forge::json j = forge::pb::to_json(rep);   // {"state": "streaming", ...}

#include "forge/json.hpp"
#include "forge/status.hpp"

#include <pb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace forge::pb {

/// What a field holds, where nanopb's own field type is not enough
/// (float and fixed32 look the same to nanopb, an enum looks like an int).
enum class FieldKind : std::uint8_t { boolean, signed_int, unsigned_int, floating, string, bytes, enumeration, message };

struct EnumValue {
    std::int32_t number;
    const char* name;
};
struct EnumNames {
    const EnumValue* values;
    std::size_t count;
};

struct Type;

struct Field {
    std::uint32_t tag;
    const char* name;       ///< as in the .proto (JSON output)
    const char* json_name;  ///< lowerCamelCase, also accepted on input
    FieldKind kind;
    const EnumNames* enums;  ///< enumeration
    const Type* message;     ///< message
};

struct Type {
    const char* name;  ///< full name, "radio.tmtc.status_reply"
    const pb_msgdesc_t* desc;
    std::size_t size;  ///< sizeof the struct
    const Field* fields;
    std::size_t field_count;
};

/// The Type of a generated message; forge-proto.py specialises it.
template <class T>
inline constexpr const Type* type_of = nullptr;

template <class T>
concept Message = type_of<T> != nullptr;

// --- binary --------------------------------------------------------------------------
/// EMSGSIZE when `out` is too small.
Status encode(const Type& type, const void* msg, std::span<std::uint8_t> out, std::size_t& written);
/// EBADMSG when the bytes are not a valid message of `type`. `msg` is reset first.
Status decode(const Type& type, std::span<const std::uint8_t> in, void* msg);

/// Bytes `msg` takes when encoded.
std::size_t encoded_size(const Type& type, const void* msg);

template <Message T>
Status encode(const T& msg, std::span<std::uint8_t> out, std::size_t& written) {
    return encode(*type_of<T>, &msg, out, written);
}
template <Message T>
Status decode(std::span<const std::uint8_t> in, T& msg) {
    return decode(*type_of<T>, in, &msg);
}

// --- JSON (proto3 mapping, field names as in the .proto, every field written) ---------
json to_json(const Type& type, const void* msg);
/// Throws forge::Error(400) with the path of the field that is wrong. `msg` is reset first.
void from_json(const Type& type, const json& j, void* msg);

template <Message T>
json to_json(const T& msg) {
    return to_json(*type_of<T>, &msg);
}
template <Message T>
T from_json(const json& j) {
    T msg{};
    from_json(*type_of<T>, j, &msg);
    return msg;
}

// --- filling messages ------------------------------------------------------------------
/// Copy text into a bounded string field. ENAMETOOLONG (nothing copied) when it does not fit.
template <std::size_t N>
Status assign(char (&dst)[N], std::string_view src) {
    if (src.size() >= N) return Status::error(module_id::pb, ENAMETOOLONG, "text longer than the field");
    std::memcpy(dst, src.data(), src.size());
    dst[src.size()] = '\0';
    return {};
}

/// Copy values into a bounded repeated field and set its count. E2BIG when they do not fit.
template <class T, std::size_t N, class Range>
Status assign(T (&dst)[N], pb_size_t& count, const Range& src) {
    const auto n = static_cast<std::size_t>(std::distance(std::begin(src), std::end(src)));
    if (n > N) return Status::error(module_id::pb, E2BIG, "more values than the field holds");
    std::copy(std::begin(src), std::end(src), dst);
    count = static_cast<pb_size_t>(n);
    return {};
}

/// The text of a bounded string field.
template <std::size_t N>
std::string_view view(const char (&s)[N]) {
    return {s, strnlen(s, N)};
}

}  // namespace forge::pb
