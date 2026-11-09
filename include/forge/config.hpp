#pragma once
// TOML configuration built on toml++, with JSON export via nlohmann/json.
//
//   auto cfg = forge::Config::load_file("/etc/mydaemon/mydaemon.toml");
//   int port      = cfg.value<int>("http.port");           // declared key: default from the schema
//   int extra     = cfg.get_or<int>("my.undeclared", 3);   // undeclared key
//   auto name     = cfg.require<std::string>("device.name");
//   auto peers    = cfg.get_or<std::vector<std::string>>("peers", {});
//   cfg.apply_override("log.level=debug");   // e.g. from `-D log.level=debug`

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace forge {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {
template <class T>
struct is_vector : std::false_type {};
template <class T, class A>
struct is_vector<std::vector<T, A>> : std::true_type {};
}  // namespace detail

class Config {
public:
    Config() = default;
    explicit Config(toml::table table, std::filesystem::path source = {});

    /// Parse a TOML file. Throws ConfigError with file:line:column on error.
    static Config load_file(const std::filesystem::path& path);
    /// Parse TOML text. Throws ConfigError on error.
    static Config parse(std::string_view text, std::string_view source_name = "<string>");

    bool empty() const { return table_.empty(); }
    bool contains(std::string_view path) const;

    /// Value at a dotted path ("a.b.c", "servers[0].host").
    /// Returns std::nullopt if missing, throws ConfigError on type mismatch.
    template <class T>
    std::optional<T> get(std::string_view path) const;

    /// Value at path or `fallback` when missing.
    template <class T>
    T get_or(std::string_view path, T fallback) const {
        auto v = get<T>(path);
        return v ? std::move(*v) : std::move(fallback);
    }

    /// Value of a declared key: the file's, or the schema's default, which
    /// forge::Application fills in (config::Schema::apply_defaults). Throws
    /// ConfigError when missing (an undeclared key: use get_or).
    template <class T>
    T value(std::string_view path) const {
        return require<T>(path);
    }

    /// Value at path; throws ConfigError when missing.
    template <class T>
    T require(std::string_view path) const {
        auto v = get<T>(path);
        if (!v) throw ConfigError("missing required config key '" + std::string(path) + "'");
        return std::move(*v);
    }

    /// Sub-table as its own Config (empty if missing or not a table).
    Config section(std::string_view path) const;

    /// Keys of the top level table.
    std::vector<std::string> keys() const;

    /// Set a value, creating intermediate tables ("a.b.c" only, no indices).
    template <class T>
    void set(std::string_view path, T&& value) {
        toml::table holder;
        holder.insert_or_assign("v", std::forward<T>(value));
        set_node(path, std::move(holder));
    }

    /// Apply "key.path=value". The value is parsed as a TOML value
    /// (42, 1.5, true, "text", [1,2]); anything unparsable is taken as a string.
    void apply_override(std::string_view assignment);

    /// Deep-merge `other` into this config; values from `other` win.
    void merge(const Config& other);

    nlohmann::json to_json() const;
    std::string to_toml() const;

    const toml::table& table() const { return table_; }
    const std::filesystem::path& source() const { return source_; }

private:
    void set_node(std::string_view path, toml::table holder);  // moves holder["v"]
    [[noreturn]] static void type_error(std::string_view path, std::string_view expected, const toml::node& node);

    template <class T>
    static std::optional<T> convert(std::string_view path, const toml::node& node);

    toml::table table_;
    std::filesystem::path source_;
};

// ---------------------------------------------------------------------------

nlohmann::json toml_to_json(const toml::node& node);

template <class T>
std::optional<T> Config::convert(std::string_view path, const toml::node& node) {
    if constexpr (std::is_same_v<T, nlohmann::json>) {
        return toml_to_json(node);
    } else if constexpr (std::is_same_v<T, Config>) {
        if (auto t = node.as_table()) return Config(*t);
        type_error(path, "table", node);
    } else if constexpr (detail::is_vector<T>::value) {
        using U = typename T::value_type;
        auto arr = node.as_array();
        if (!arr) type_error(path, "array", node);
        T out;
        out.reserve(arr->size());
        for (std::size_t i = 0; i < arr->size(); ++i) {
            const std::string elem_path = std::string(path) + "[" + std::to_string(i) + "]";
            out.push_back(*convert<U>(elem_path, *arr->get(i)));
        }
        return out;
    } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
        if (auto s = node.value<std::string>()) return std::filesystem::path(*s);
        type_error(path, "string", node);
    } else if constexpr (std::is_same_v<T, bool>) {
        if (node.is_boolean()) return node.as_boolean()->get();
        type_error(path, "boolean", node);
    } else if constexpr (std::is_integral_v<T>) {
        if (!node.is_integer()) type_error(path, "integer", node);
        if (auto v = node.value<T>()) return v;
        throw ConfigError("config key '" + std::string(path) + "' is out of range");
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!node.is_number()) type_error(path, "number", node);
        return node.value<T>();
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!node.is_string()) type_error(path, "string", node);
        return node.as_string()->get();
    } else {
        // Anything toml++ knows how to read (dates, times, ...).
        if (auto v = node.value<T>()) return v;
        type_error(path, "compatible value", node);
    }
}

template <class T>
std::optional<T> Config::get(std::string_view path) const {
    auto view = table_.at_path(path);
    if (!view) return std::nullopt;
    return convert<T>(path, *view.node());
}

}  // namespace forge
