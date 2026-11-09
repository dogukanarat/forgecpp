#include "forge/pb.hpp"

#include "forge/base64.hpp"
#include "forge/error.hpp"

#include <pb_common.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <cerrno>
#include <cmath>
#include <format>
#include <limits>
#include <string>

namespace forge::pb {
namespace {

// A field of one message, found by its tag.
struct Slot {
    pb_field_iter_t it;
    bool found;
};

Slot slot_of(const Type& type, void* msg, std::uint32_t tag) {
    Slot s{};
    s.found = pb_field_iter_begin(&s.it, type.desc, msg) && pb_field_iter_find(&s.it, tag);
    return s;
}

bool is_oneof(const pb_field_iter_t& it) { return PB_HTYPE(it.type) == PB_HTYPE_ONEOF; }
bool is_repeated(const pb_field_iter_t& it) { return PB_HTYPE(it.type) == PB_HTYPE_REPEATED; }
bool has_flag(const pb_field_iter_t& it) { return PB_HTYPE(it.type) == PB_HTYPE_OPTIONAL && it.pSize != nullptr; }

std::int64_t read_signed(const void* p, std::size_t size) {
    switch (size) {
        case 1: return *static_cast<const std::int8_t*>(p);
        case 2: return *static_cast<const std::int16_t*>(p);
        case 4: return *static_cast<const std::int32_t*>(p);
        default: return *static_cast<const std::int64_t*>(p);
    }
}

std::uint64_t read_unsigned(const void* p, std::size_t size) {
    switch (size) {
        case 1: return *static_cast<const std::uint8_t*>(p);
        case 2: return *static_cast<const std::uint16_t*>(p);
        case 4: return *static_cast<const std::uint32_t*>(p);
        default: return *static_cast<const std::uint64_t*>(p);
    }
}

template <class T>
void write_as(void* p, T v) {
    std::memcpy(p, &v, sizeof(v));
}

void write_signed(void* p, std::size_t size, std::int64_t v) {
    switch (size) {
        case 1: write_as(p, static_cast<std::int8_t>(v)); break;
        case 2: write_as(p, static_cast<std::int16_t>(v)); break;
        case 4: write_as(p, static_cast<std::int32_t>(v)); break;
        default: write_as(p, v); break;
    }
}

void write_unsigned(void* p, std::size_t size, std::uint64_t v) {
    switch (size) {
        case 1: write_as(p, static_cast<std::uint8_t>(v)); break;
        case 2: write_as(p, static_cast<std::uint16_t>(v)); break;
        case 4: write_as(p, static_cast<std::uint32_t>(v)); break;
        default: write_as(p, v); break;
    }
}

// Bytes are either pb_bytes_array_t (size + data) or, with fixed_length, a plain array.
bool fixed_length(const pb_field_iter_t& it) { return PB_LTYPE(it.type) == PB_LTYPE_FIXED_LENGTH_BYTES; }
std::size_t bytes_capacity(const pb_field_iter_t& it) {
    return fixed_length(it) ? it.data_size : it.data_size - offsetof(pb_bytes_array_t, bytes);
}

// --- to JSON -----------------------------------------------------------------------------
json value_to_json(const Field& f, const pb_field_iter_t& it, const void* p) {
    const std::size_t size = it.data_size;
    switch (f.kind) {
        case FieldKind::boolean: return *static_cast<const bool*>(p);
        case FieldKind::signed_int: {
            const auto v = read_signed(p, size);
            return size == 8 ? json(std::to_string(v)) : json(v);  // proto3 JSON: 64-bit as text
        }
        case FieldKind::unsigned_int: {
            const auto v = read_unsigned(p, size);
            return size == 8 ? json(std::to_string(v)) : json(v);
        }
        case FieldKind::floating: {
            const double v = size == 4 ? *static_cast<const float*>(p) : *static_cast<const double*>(p);
            if (std::isnan(v)) return "NaN";
            if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
            return v;
        }
        case FieldKind::string:
            return std::string(static_cast<const char*>(p), strnlen(static_cast<const char*>(p), size));
        case FieldKind::bytes: {
            if (fixed_length(it)) return base64_encode({static_cast<const char*>(p), size});
            const auto* b = static_cast<const pb_bytes_array_t*>(p);
            return base64_encode({reinterpret_cast<const char*>(b->bytes), b->size});
        }
        case FieldKind::enumeration: {
            const auto v = static_cast<std::int32_t>(read_signed(p, size));
            for (std::size_t i = 0; i < f.enums->count; ++i) {
                if (f.enums->values[i].number == v) return f.enums->values[i].name;
            }
            return v;  // a value from a newer peer
        }
        case FieldKind::message: return to_json(*f.message, p);
    }
    return nullptr;
}

// --- from JSON ---------------------------------------------------------------------------
[[noreturn]] void bad(const std::string& path, std::string_view why) {
    throw Error(400, std::format("{}: {}", path, why));
}

template <class Int>
Int integer(const json& v, const std::string& path) {
    if (v.is_number_integer()) {
        if constexpr (std::is_signed_v<Int>) {
            if (v.is_number_unsigned() && v.get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX)) {
                bad(path, "out of range");
            }
            return static_cast<Int>(v.get<std::int64_t>());
        } else {
            if (!v.is_number_unsigned() && v.get<std::int64_t>() < 0) bad(path, "out of range");
            return static_cast<Int>(v.get<std::uint64_t>());
        }
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (d != std::floor(d)) bad(path, "not an integer");
        if (d < static_cast<double>(std::numeric_limits<Int>::min()) ||
            d > static_cast<double>(std::numeric_limits<Int>::max())) {
            bad(path, "out of range");
        }
        return static_cast<Int>(d);
    }
    if (v.is_string()) {
        const auto& s = v.get_ref<const std::string&>();
        try {
            std::size_t used = 0;
            if constexpr (std::is_signed_v<Int>) {
                const long long n = std::stoll(s, &used);
                if (used != s.size()) bad(path, "not an integer");
                return static_cast<Int>(n);
            } else {
                if (!s.empty() && s[0] == '-') bad(path, "out of range");
                const unsigned long long n = std::stoull(s, &used);
                if (used != s.size()) bad(path, "not an integer");
                return static_cast<Int>(n);
            }
        } catch (const std::logic_error&) {
            bad(path, "not an integer");
        }
    }
    bad(path, "not an integer");
}

void check_range(std::int64_t v, std::size_t size, const std::string& path) {
    if (size >= 8) return;
    const std::int64_t lim = std::int64_t{1} << (size * 8 - 1);
    if (v < -lim || v >= lim) bad(path, "out of range");
}

void check_range(std::uint64_t v, std::size_t size, const std::string& path) {
    if (size >= 8) return;
    if (v >= (std::uint64_t{1} << (size * 8))) bad(path, "out of range");
}

void value_from_json(const Field& f, const pb_field_iter_t& it, void* p, const json& v, const std::string& path) {
    const std::size_t size = it.data_size;
    switch (f.kind) {
        case FieldKind::boolean:
            if (!v.is_boolean()) bad(path, "not true or false");
            *static_cast<bool*>(p) = v.get<bool>();
            return;
        case FieldKind::signed_int: {
            const auto n = integer<std::int64_t>(v, path);
            check_range(n, size, path);
            write_signed(p, size, n);
            return;
        }
        case FieldKind::unsigned_int: {
            const auto n = integer<std::uint64_t>(v, path);
            check_range(n, size, path);
            write_unsigned(p, size, n);
            return;
        }
        case FieldKind::floating: {
            double d = 0;
            if (v.is_number()) {
                d = v.get<double>();
            } else if (v == "NaN") {
                d = std::numeric_limits<double>::quiet_NaN();
            } else if (v == "Infinity") {
                d = std::numeric_limits<double>::infinity();
            } else if (v == "-Infinity") {
                d = -std::numeric_limits<double>::infinity();
            } else {
                bad(path, "not a number");
            }
            if (size == 4) {
                write_as(p, static_cast<float>(d));
            } else {
                write_as(p, d);
            }
            return;
        }
        case FieldKind::string: {
            if (!v.is_string()) bad(path, "not text");
            const auto& s = v.get_ref<const std::string&>();
            if (s.size() >= size) bad(path, std::format("longer than {} bytes", size - 1));
            std::memcpy(p, s.data(), s.size());
            static_cast<char*>(p)[s.size()] = '\0';
            return;
        }
        case FieldKind::bytes: {
            if (!v.is_string()) bad(path, "not base64 text");
            const auto bytes = base64_decode(v.get_ref<const std::string&>());
            if (!bytes) bad(path, "not base64 text");
            const auto cap = bytes_capacity(it);
            if (fixed_length(it) ? bytes->size() != cap : bytes->size() > cap) {
                bad(path, std::format("{} bytes, the field holds {}", bytes->size(), cap));
            }
            if (fixed_length(it)) {
                std::memcpy(p, bytes->data(), bytes->size());
            } else {
                auto* b = static_cast<pb_bytes_array_t*>(p);
                b->size = static_cast<pb_size_t>(bytes->size());
                std::memcpy(b->bytes, bytes->data(), bytes->size());
            }
            return;
        }
        case FieldKind::enumeration: {
            std::int64_t n = 0;
            if (v.is_string()) {
                const auto& name = v.get_ref<const std::string&>();
                const auto* e = std::find_if(f.enums->values, f.enums->values + f.enums->count,
                                             [&](const EnumValue& ev) { return name == ev.name; });
                if (e == f.enums->values + f.enums->count) bad(path, std::format("unknown value '{}'", name));
                n = e->number;
            } else {
                n = integer<std::int32_t>(v, path);
            }
            write_signed(p, size, n);
            return;
        }
        case FieldKind::message:
            if (!v.is_object()) bad(path, "not an object");
            from_json(*f.message, v, p);
            return;
    }
}

void fill(const Type& type, const json& j, void* msg, const std::string& prefix) {
    if (!j.is_object()) bad(prefix.empty() ? type.name : prefix, "not an object");
    for (const auto& [key, value] : j.items()) {
        const Field* f = std::find_if(type.fields, type.fields + type.field_count,
                                      [&](const Field& x) { return key == x.name || key == x.json_name; });
        const std::string path = prefix.empty() ? key : prefix + "." + key;
        if (f == type.fields + type.field_count) bad(path, std::format("not a field of {}", type.name));
        if (value.is_null()) continue;
        auto s = slot_of(type, msg, f->tag);
        if (!s.found) bad(path, "not in the message table");
        auto& it = s.it;
        if (is_oneof(it)) {
            auto& which = *static_cast<pb_size_t*>(it.pSize);
            if (which != 0 && which != it.tag) bad(path, "another member of its oneof is already set");
            which = it.tag;
        } else if (has_flag(it)) {
            *static_cast<bool*>(it.pSize) = true;
        }
        if (is_repeated(it)) {
            if (!value.is_array()) bad(path, "not a list");
            if (value.size() > it.array_size) bad(path, std::format("more than {} values", it.array_size));
            for (std::size_t i = 0; i < value.size(); ++i) {
                void* p = static_cast<char*>(it.pData) + i * it.data_size;
                const std::string item = std::format("{}[{}]", path, i);
                if (f->kind == FieldKind::message) {
                    if (!value[i].is_object()) bad(item, "not an object");
                    fill(*f->message, value[i], p, item);
                } else {
                    value_from_json(*f, it, p, value[i], item);
                }
            }
            if (it.pSize != nullptr) *static_cast<pb_size_t*>(it.pSize) = static_cast<pb_size_t>(value.size());
        } else if (f->kind == FieldKind::message) {
            if (!value.is_object()) bad(path, "not an object");
            fill(*f->message, value, it.pData, path);
        } else {
            value_from_json(*f, it, it.pData, value, path);
        }
    }
}

}  // namespace

Status encode(const Type& type, const void* msg, std::span<std::uint8_t> out, std::size_t& written) {
    pb_ostream_t s = pb_ostream_from_buffer(out.data(), out.size());
    if (!pb_encode(&s, type.desc, msg)) return Status::error(module_id::pb, EMSGSIZE, PB_GET_ERROR(&s));
    written = s.bytes_written;
    return {};
}

std::size_t encoded_size(const Type& type, const void* msg) {
    std::size_t n = 0;
    return pb_get_encoded_size(&n, type.desc, msg) ? n : 0;
}

Status decode(const Type& type, std::span<const std::uint8_t> in, void* msg) {
    std::memset(msg, 0, type.size);
    pb_istream_t s = pb_istream_from_buffer(in.data(), in.size());
    if (!pb_decode(&s, type.desc, msg)) return Status::error(module_id::pb, EBADMSG, PB_GET_ERROR(&s));
    return {};
}

json to_json(const Type& type, const void* msg) {
    json j = json::object();
    auto* m = const_cast<void*>(msg);  // nanopb's iterator takes a non-const pointer; nothing is written
    for (std::size_t i = 0; i < type.field_count; ++i) {
        const Field& f = type.fields[i];
        auto s = slot_of(type, m, f.tag);
        if (!s.found) continue;
        const auto& it = s.it;
        if (is_oneof(it) && *static_cast<const pb_size_t*>(it.pSize) != it.tag) continue;
        if (has_flag(it) && !*static_cast<const bool*>(it.pSize)) continue;
        if (is_repeated(it)) {
            const std::size_t count = it.pSize ? *static_cast<const pb_size_t*>(it.pSize) : it.array_size;
            json list = json::array();
            for (std::size_t k = 0; k < count; ++k) {
                list.push_back(value_to_json(f, it, static_cast<const char*>(it.pData) + k * it.data_size));
            }
            j[f.name] = std::move(list);
        } else {
            j[f.name] = value_to_json(f, it, it.pData);
        }
    }
    return j;
}

void from_json(const Type& type, const json& j, void* msg) {
    std::memset(msg, 0, type.size);
    fill(type, j, msg, "");
}

json schema(const Type& type) {
    static constexpr const char* kinds[] = {"bool", "int", "uint", "float", "string", "bytes", "enum", "message"};
    std::vector<char> blank(type.size);  // nanopb's iterator needs a message to point into
    json fields = json::array();
    for (std::size_t i = 0; i < type.field_count; ++i) {
        const Field& f = type.fields[i];
        auto s = slot_of(type, blank.data(), f.tag);
        if (!s.found) continue;
        const auto& it = s.it;
        json d{{"name", f.name}, {"tag", f.tag}, {"kind", kinds[static_cast<int>(f.kind)]}};
        switch (f.kind) {
            case FieldKind::signed_int:
            case FieldKind::unsigned_int:
            case FieldKind::floating: d["bits"] = it.data_size * 8; break;
            // nanopb's max_size of a string counts the terminating NUL: the text is one shorter.
            case FieldKind::string: d["max_length"] = it.data_size - 1; break;
            case FieldKind::bytes: d["max_size"] = bytes_capacity(it); break;
            case FieldKind::enumeration: {
                json values = json::array();
                for (std::size_t k = 0; k < f.enums->count; ++k) values.push_back(f.enums->values[k].name);
                d["values"] = std::move(values);
                break;
            }
            case FieldKind::message: d["fields"] = schema(*f.message)["fields"]; break;
            default: break;
        }
        if (is_repeated(it)) {
            d["repeated"] = true;
            d["max_count"] = it.array_size;
        }
        fields.push_back(std::move(d));
    }
    return {{"name", type.name}, {"fields", std::move(fields)}};
}

}  // namespace forge::pb
