#pragma once
// Users and roles, shared by every way into an application: web (HTTP basic
// or bearer token), forge.v1 requests (Request.token), telnet (login) and
// the local console (always admin).
//
//   [auth]
//   enabled = true
//   default_role = "viewer"          # role without credentials (when enabled)
//   [auth.users.alice]
//   password = "secret"               # HTTP basic auth and telnet login
//   token = "abc123"                  # HTTP "Authorization: Bearer" and Request.token
//   role = "admin"                    # viewer | operator | admin
//
// Roles: viewer reads everything; operator also changes controls, presses
// buttons, calls commands, acknowledges alarms and takes leases; admin may
// also change controls marked .role(admin), log levels and reload.
// With [auth] disabled every caller is admin (as before). The older
// [http.auth] user/password/token still works and defines an admin user.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

class Config;

enum class Role { viewer = 0, operator_ = 1, admin = 2 };

const char* to_string(Role role);
/// "viewer" | "operator" | "admin"; throws std::invalid_argument.
Role parse_role(std::string_view text);

/// Who is calling, and through what.
struct Caller {
    std::string channel = "internal";  ///< web | zmq | console | telnet | http-rpc | internal
    std::string client;                ///< address or client name
    std::string user;                  ///< authenticated user, empty if anonymous
    Role role = Role::admin;
    /// "alice@web 10.0.0.2", used as lease holder and in messages.
    std::string describe() const;
};

class Auth {
public:
    struct User {
        std::string name, password, token;
        Role role = Role::viewer;
    };

    void load(const Config& cfg);
    bool enabled() const { return enabled_; }
    Role default_role() const { return enabled_ ? default_role_ : Role::admin; }
    const std::vector<User>& users() const { return users_; }

    /// By token (Request.token, HTTP bearer).
    std::optional<User> by_token(std::string_view token) const;
    /// By name and password (HTTP basic, telnet login).
    std::optional<User> by_password(std::string_view name, std::string_view password) const;
    /// From an HTTP Authorization header value ("Basic ..." / "Bearer ...").
    std::optional<User> by_http_header(std::string_view authorization) const;

private:
    bool enabled_ = false;
    Role default_role_ = Role::viewer;
    std::vector<User> users_;
};

}  // namespace forge
