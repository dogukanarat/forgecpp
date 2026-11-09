#pragma once
// TOML configuration. The parser (toml++) stays inside the library: this
// header only needs nlohmann/json, which is forgecpp's JSON type everywhere.
//
//   auto cfg = forge::Config::load_file("/etc/mydaemon/mydaemon.toml");
//   int port      = cfg.value<int>("http.port");           // declared key: default from the schema
//   int extra     = cfg.get_or<int>("my.undeclared", 3);   // undeclared key
//   auto name     = cfg.require<std::string>("device.name");
//   auto peers    = cfg.get_or<std::vector<std::string>>("peers", {});
//   cfg.apply_override("log.level=debug");   // e.g. from `-D log.level=debug`

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
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
    struct Impl;  ///< the parsed table (toml++), defined in the library

    Config();
    Config(const Config& other);
    Config(Config&& other) noexcept;
    Config& operator=(const Config& other);
    Config& operator=(Config&& other) noexcept;
    ~Config();

    /// Parse a TOML file. Throws ConfigError with file:line:column on error.
    static Config load_file(const std::filesystem::path& path);
    /// Parse TOML text. Throws ConfigError on error.
    static Config parse(std::string_view text, std::string_view source_name = "<string>");

    bool empty() const;
    bool contains(std::string_view path) const;

    /// Value at a dotted path ("a.b.c", "servers[0].host").
    /// Returns std::nullopt if missing, throws ConfigError on type mismatch.
    template <class T>
    [[nodiscard]] std::optional<T> get(std::string_view path) const {
        auto v = node(path);
        if (!v) return std::nullopt;
        return convert<T>(path, *v);
    }

    /// Value at path or `fallback` when missing.
    template <class T>
    [[nodiscard]] T get_or(std::string_view path, T fallback) const {
        auto v = get<T>(path);
        return v ? std::move(*v) : std::move(fallback);
    }

    /// Value of a declared key: the file's, or the schema's default, which
    /// forge::Application fills in (config::Schema::apply_defaults). Throws
    /// ConfigError when missing (an undeclared key: use get_or).
    template <class T>
    [[nodiscard]] T value(std::string_view path) const {
        return require<T>(path);
    }

    /// Value at path; throws ConfigError when missing.
    template <class T>
    [[nodiscard]] T require(std::string_view path) const {
        auto v = get<T>(path);
        if (!v) throw ConfigError("missing required config key '" + std::string(path) + "'");
        return std::move(*v);
    }

    /// Sub-table as its own Config (empty if missing or not a table).
    Config section(std::string_view path) const;

    /// Keys of the top level table.
    std::vector<std::string> keys() const;

    /// Set a value (any JSON value: a number, text, a list, a table), creating
    /// intermediate tables ("a.b.c" only, no indices).
    void set(std::string_view path, const nlohmann::json& value);

    /// Apply "key.path=value". The value is parsed as a TOML value
    /// (42, 1.5, true, "text", [1,2]); anything unparsable is taken as a string.
    void apply_override(std::string_view assignment);

    /// Deep-merge `other` into this config; values from `other` win.
    void merge(const Config& other);

    nlohmann::json to_json() const;
    std::string to_toml() const;

    const std::filesystem::path& source() const;

private:
    explicit Config(std::unique_ptr<Impl> impl);

    /// The value at `path` as JSON (nullopt: no such key).
    std::optional<nlohmann::json> node(std::string_view path) const;
    [[noreturn]] static void type_error(std::string_view path, std::string_view expected, const nlohmann::json& got);

    template <class T>
    static T convert(std::string_view path, const nlohmann::json& v);

    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------

template <class T>
T Config::convert(std::string_view path, const nlohmann::json& v) {
    if constexpr (std::is_same_v<T, nlohmann::json>) {
        return v;
    } else if constexpr (detail::is_vector<T>::value) {
        using U = typename T::value_type;
        if (!v.is_array()) type_error(path, "array", v);
        T out;
        out.reserve(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            out.push_back(convert<U>(std::string(path) + "[" + std::to_string(i) + "]", v[i]));
        }
        return out;
    } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
        if (!v.is_string()) type_error(path, "string", v);
        return std::filesystem::path(v.get<std::string>());
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!v.is_boolean()) type_error(path, "boolean", v);
        return v.get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (!v.is_number_integer()) type_error(path, "integer", v);
        const auto wide = v.get<std::int64_t>();
        if (wide < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
            (std::is_signed_v<T> ? wide > static_cast<std::int64_t>(std::numeric_limits<T>::max())
                                 : static_cast<std::uint64_t>(wide) > std::numeric_limits<T>::max())) {
            throw ConfigError("config key '" + std::string(path) + "' is out of range");
        }
        return static_cast<T>(wide);
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.is_number()) type_error(path, "number", v);
        return static_cast<T>(v.get<double>());
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!v.is_string()) type_error(path, "string", v);
        return v.get<std::string>();
    } else {
        // Anything JSON converts to (dates and times are strings).
        return v.get<T>();
    }
}

}  // namespace forge
