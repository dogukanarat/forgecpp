#pragma once
// forge::Error: a failed request, independent of the channel it came in on.
//
// ControlService, the control registry (web::Dashboard) and application code
// throw it; each channel maps it to its own form:
//   HTTP          status code, {"error": message} or `details` as the body
//   commands      NACK with the errno of the status (forge::errno_for_http), from
//                 module() or, if not set, the command's module (tmtc::module_of)
//   console       "error: message"
//
// http::HttpError derives from it, so code that throws HttpError keeps
// working and `catch (const forge::Error&)` catches both.

#include "forge/json.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace forge {

class Error : public std::runtime_error {
public:
    /// `status` follows HTTP: 400 bad request, 401/403 not allowed, 404 unknown,
    /// 409 conflict (leased, wrong state), 500 failed, 503 busy / unavailable.
    Error(int status, const std::string& message, json details = nullptr)
        : std::runtime_error(message), status_(status), details_(std::move(details)) {}
    int status() const { return status_; }
    /// Structured details; when set, HTTP answers with them as the body.
    const json& details() const { return details_; }
    /// The module that refuses (forge::module_id), for a NACK; 0: the module
    /// of the command whose handler threw it.
    std::uint16_t module() const { return module_; }
    Error& module(std::uint16_t id) {
        module_ = id;
        return *this;
    }

private:
    int status_;
    json details_;
    std::uint16_t module_ = 0;
};

}  // namespace forge
