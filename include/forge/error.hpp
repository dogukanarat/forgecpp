#pragma once
// forge::Error: a failed request, independent of the channel it came in on.
//
// ControlService, the control registry (web::Dashboard) and application code
// throw it; each channel maps it to its own form:
//   HTTP          status code, {"error": message} or `details` as the body
//   commands      NACK with the errno of the status (forge::errno_for_http)
//   console       "error: message"
//
// http::HttpError derives from it, so code that throws HttpError keeps
// working and `catch (const forge::Error&)` catches both.

#include "forge/json.hpp"

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

private:
    int status_;
    json details_;
};

}  // namespace forge
