#pragma once
// forge.v1 protobuf helpers: the message standard shared by every forgecpp
// application (see proto/forge/v1/*.proto).
//
//   forge::v1::Header h = forge::proto::make_header("spectrum", "sa.v1.Spectrum");
//   google::protobuf::Value v = forge::proto::to_value(json(42));
//   std::string text = forge::proto::to_json_string(reply);   // for logs / tools

#include "forge/json.hpp"
#include "forge/v1/common.pb.h"
#include "forge/v1/control.pb.h"
#include "forge/v1/observability.pb.h"

#include <google/protobuf/message.h>
#include <google/protobuf/struct.pb.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace forge::proto {

/// Protocol name announced in Info.protocol.
inline constexpr const char* kProtocol = "forge.v1";

/// Set by Application at start-up: fills Header.source/host/pid.
void set_source(std::string name);
const std::string& source();
const std::string& host();

std::int64_t now_unix_ns();

/// Header for a published message (source, host, pid, time, trace context of
/// the current thread). `seq` is filled by the sender.
v1::Header make_header(std::string_view topic, std::string_view type);

// --- dynamic values -----------------------------------------------------------
google::protobuf::Value to_value(const json& j);
json to_json(const google::protobuf::Value& v);
google::protobuf::Struct to_struct(const json& object);
json to_json(const google::protobuf::Struct& s);

// --- JSON mapping (proto3 canonical JSON, field names as in the .proto) -------------
/// Throws std::invalid_argument on errors.
std::string to_json_string(const google::protobuf::Message& m, bool pretty = false);
void from_json_string(std::string_view text, google::protobuf::Message* m);
json to_json(const google::protobuf::Message& m);

// --- conversions of framework types --------------------------------------------------
v1::Severity to_proto(int severity);  ///< forge::Severity as int (info=0 ... critical=3)
const char* severity_name(v1::Severity s);

/// "Reply with status": ok=false plus error/status.
v1::Reply error_reply(int status, std::string message);

}  // namespace forge::proto
