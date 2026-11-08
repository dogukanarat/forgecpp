#include "forge/auth.hpp"

#include "forge/base64.hpp"

#include "forge/config.hpp"

#include <format>

#include <stdexcept>

namespace forge {
namespace {

bool equal_consttime(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}


}  // namespace

const char* to_string(Role role) {
    switch (role) {
        case Role::viewer: return "viewer";
        case Role::operator_: return "operator";
        case Role::admin: return "admin";
    }
    return "viewer";
}

Role parse_role(std::string_view text) {
    if (text == "viewer") return Role::viewer;
    if (text == "operator") return Role::operator_;
    if (text == "admin") return Role::admin;
    throw std::invalid_argument("unknown role '" + std::string(text) + "' (viewer|operator|admin)");
}

std::string Caller::describe() const {
    std::string s = user.empty() ? channel : user + "@" + channel;
    if (!client.empty()) s += " " + client;
    return s;
}

void Auth::load(const Config& cfg) {
    users_.clear();
    enabled_ = cfg.value<bool>("auth.enabled");
    default_role_ = parse_role(cfg.value<std::string>("auth.default_role"));
    const auto users = cfg.section("auth.users").to_json();
    if (users.is_object()) {
        for (const auto& [name, u] : users.items()) {
            if (!u.is_object()) continue;
            User user;
            user.name = name;
            user.password = u.value("password", std::string());
            user.token = u.value("token", std::string());
            user.role = parse_role(u.value("role", std::string("viewer")));
            users_.push_back(user);
        }
    }
    // Backwards compatible single web user.
    User legacy;
    legacy.name = cfg.get_or<std::string>("http.auth.user", "");
    legacy.password = cfg.get_or<std::string>("http.auth.password", "");
    legacy.token = cfg.get_or<std::string>("http.auth.token", "");
    legacy.role = Role::admin;
    if (!legacy.name.empty() || !legacy.token.empty()) {
        if (legacy.name.empty()) legacy.name = "admin";
        users_.push_back(legacy);
        enabled_ = true;
    }
    if (enabled_ && users_.empty()) throw ConfigError("[auth] is enabled but no [auth.users.<name>] is defined");
}

std::optional<Auth::User> Auth::by_token(std::string_view token) const {
    if (token.empty()) return std::nullopt;
    for (const auto& u : users_) {
        if (!u.token.empty() && equal_consttime(u.token, token)) return u;
    }
    return std::nullopt;
}

std::optional<Auth::User> Auth::by_password(std::string_view name, std::string_view password) const {
    for (const auto& u : users_) {
        if (u.name == name && !u.password.empty() && equal_consttime(u.password, password)) return u;
    }
    return std::nullopt;
}

std::optional<Auth::User> Auth::by_http_header(std::string_view h) const {
    if (h.rfind("Bearer ", 0) == 0) return by_token(h.substr(7));
    if (h.rfind("Basic ", 0) == 0) {
        const auto decoded = base64_decode(h.substr(6)).value_or(std::string());
        const auto colon = decoded.find(':');
        if (colon == std::string::npos) return std::nullopt;
        return by_password(decoded.substr(0, colon), decoded.substr(colon + 1));
    }
    return std::nullopt;
}

}  // namespace forge
