#include "forge/proto.hpp"

#include "forge/trace.hpp"

#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <unistd.h>

namespace forge::proto {
namespace {

struct Process {
    std::mutex mutex;  // source: set by every Application in the process (tests run several)
    std::string source = "app";
    std::string host;
    std::uint32_t pid = 0;
};

Process& process() {
    static Process p;
    static std::once_flag once;
    std::call_once(once, [] {
        char buf[256] = {};
        if (::gethostname(buf, sizeof(buf) - 1) == 0) p.host = buf;
        p.pid = static_cast<std::uint32_t>(::getpid());
    });
    return p;
}

}  // namespace

void set_source(std::string name) {
    auto& p = process();
    std::lock_guard lock(p.mutex);
    p.source = std::move(name);
}
std::string source() {
    auto& p = process();
    std::lock_guard lock(p.mutex);
    return p.source;
}
const std::string& host() { return process().host; }

std::int64_t now_unix_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

v1::Header make_header(std::string_view topic, std::string_view type) {
    v1::Header h;
    const auto& p = process();
    h.set_source(source());
    h.set_host(p.host);
    h.set_pid(p.pid);
    h.set_topic(std::string(topic));
    h.set_type(std::string(type));
    h.set_time_unix_ns(now_unix_ns());
    const auto ctx = trace::current();
    if (ctx.valid()) *h.mutable_trace() = trace::to_proto(ctx);
    return h;
}

google::protobuf::Value to_value(const json& j) {
    google::protobuf::Value v;
    switch (j.type()) {
        case json::value_t::null:
        case json::value_t::discarded:
            v.set_null_value(google::protobuf::NULL_VALUE);
            break;
        case json::value_t::boolean:
            v.set_bool_value(j.get<bool>());
            break;
        case json::value_t::number_integer:
        case json::value_t::number_unsigned:
        case json::value_t::number_float:
            v.set_number_value(j.get<double>());
            break;
        case json::value_t::string:
            v.set_string_value(j.get<std::string>());
            break;
        case json::value_t::array: {
            auto* list = v.mutable_list_value();
            for (const auto& e : j) *list->add_values() = to_value(e);
            break;
        }
        case json::value_t::object:
            *v.mutable_struct_value() = to_struct(j);
            break;
        case json::value_t::binary:
            v.set_null_value(google::protobuf::NULL_VALUE);
            break;
    }
    return v;
}

json to_json(const google::protobuf::Value& v) {
    switch (v.kind_case()) {
        case google::protobuf::Value::kBoolValue:
            return v.bool_value();
        case google::protobuf::Value::kNumberValue: {
            const double d = v.number_value();
            // Integral numbers stay integers in JSON (42, not 42.0).
            if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e15) return static_cast<std::int64_t>(d);
            return d;
        }
        case google::protobuf::Value::kStringValue:
            return v.string_value();
        case google::protobuf::Value::kListValue: {
            json a = json::array();
            for (const auto& e : v.list_value().values()) a.push_back(to_json(e));
            return a;
        }
        case google::protobuf::Value::kStructValue:
            return to_json(v.struct_value());
        default:
            return nullptr;
    }
}

google::protobuf::Struct to_struct(const json& object) {
    google::protobuf::Struct s;
    if (!object.is_object()) return s;
    for (const auto& [k, v] : object.items()) (*s.mutable_fields())[k] = to_value(v);
    return s;
}

json to_json(const google::protobuf::Struct& s) {
    json o = json::object();
    for (const auto& [k, v] : s.fields()) o[k] = to_json(v);
    return o;
}

std::string to_json_string(const google::protobuf::Message& m, bool pretty) {
    google::protobuf::util::JsonPrintOptions o;
    o.add_whitespace = pretty;
    o.preserve_proto_field_names = true;
    o.always_print_primitive_fields = false;
    std::string out;
    const auto st = google::protobuf::util::MessageToJsonString(m, &out, o);
    if (!st.ok()) throw std::invalid_argument("cannot convert " + m.GetTypeName() + " to JSON: " + st.ToString());
    return out;
}

void from_json_string(std::string_view text, google::protobuf::Message* m) {
    google::protobuf::util::JsonParseOptions o;
    o.ignore_unknown_fields = false;
    const auto st = google::protobuf::util::JsonStringToMessage(std::string(text), m, o);
    if (!st.ok()) {
        throw std::invalid_argument("invalid " + m->GetTypeName() + " JSON: " + std::string(st.message()));
    }
}

json to_json(const google::protobuf::Message& m) { return json::parse(to_json_string(m)); }

v1::Severity to_proto(int severity) {
    switch (severity) {
        case 0: return v1::SEVERITY_INFO;
        case 1: return v1::SEVERITY_WARNING;
        case 2: return v1::SEVERITY_ERROR;
        case 3: return v1::SEVERITY_CRITICAL;
        default: return v1::SEVERITY_UNSPECIFIED;
    }
}

const char* severity_name(v1::Severity s) {
    switch (s) {
        case v1::SEVERITY_INFO: return "info";
        case v1::SEVERITY_WARNING: return "warning";
        case v1::SEVERITY_ERROR: return "error";
        case v1::SEVERITY_CRITICAL: return "critical";
        default: return "unspecified";
    }
}

v1::Reply error_reply(int status, std::string message) {
    v1::Reply r;
    r.set_ok(false);
    r.set_status(status);
    r.set_error(std::move(message));
    return r;
}

}  // namespace forge::proto
