#include "forge/config.hpp"

#include <toml++/toml.hpp>

#include <sstream>

namespace forge {

struct Config::Impl {
    toml::table table;
    std::filesystem::path source;
};

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> split_dotted(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto dot = path.find('.', start);
        const auto part = trim(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
        if (part.empty()) throw ConfigError("invalid config path '" + std::string(path) + "'");
        parts.emplace_back(part);
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

std::string error_text(const toml::parse_error& err, std::string_view source) {
    std::ostringstream os;
    os << source << ":" << err.source().begin.line << ":" << err.source().begin.column << ": " << err.description();
    return os.str();
}

void merge_tables(toml::table& dst, const toml::table& src) {
    for (const auto& [key, node] : src) {
        auto* dst_node = dst.get(key);
        if (dst_node && dst_node->is_table() && node.is_table()) {
            merge_tables(*dst_node->as_table(), *node.as_table());
        } else {
            dst.insert_or_assign(key, node);
        }
    }
}

// Straight from the parsed nodes (no text in between): a scalar read costs a
// few instructions. Dates and times, which JSON does not have, become text.
nlohmann::json toml_to_json(const toml::node& node) {
    const auto text = [](const auto& value) {
        std::ostringstream os;
        os << value;
        return os.str();
    };
    switch (node.type()) {
        case toml::node_type::table: {
            auto out = nlohmann::json::object();
            for (const auto& [key, value] : *node.as_table()) out[std::string(key.str())] = toml_to_json(value);
            return out;
        }
        case toml::node_type::array: {
            auto out = nlohmann::json::array();
            for (const auto& value : *node.as_array()) out.push_back(toml_to_json(value));
            return out;
        }
        case toml::node_type::string: return node.as_string()->get();
        case toml::node_type::integer: return node.as_integer()->get();
        case toml::node_type::floating_point: return node.as_floating_point()->get();
        case toml::node_type::boolean: return node.as_boolean()->get();
        case toml::node_type::date: return text(node.as_date()->get());
        case toml::node_type::time: return text(node.as_time()->get());
        case toml::node_type::date_time: return text(node.as_date_time()->get());
        default: return nullptr;
    }
}

// A JSON value as TOML text (an inline table for an object): toml++ parses
// it back, so one code path builds every kind of value.
std::string toml_literal(const nlohmann::json& v) {
    if (v.is_object()) {
        std::string out = "{ ";
        bool first = true;
        for (const auto& [k, item] : v.items()) {
            if (item.is_null()) continue;  // TOML has no null
            out += (first ? "" : ", ") + nlohmann::json(k).dump() + " = " + toml_literal(item);
            first = false;
        }
        return out + " }";
    }
    if (v.is_array()) {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); ++i) out += (i ? ", " : "") + toml_literal(v[i]);
        return out + "]";
    }
    if (v.is_null()) throw ConfigError("a configuration value cannot be null");
    return v.dump();  // numbers, booleans and strings are the same in JSON and TOML
}

std::unique_ptr<Config::Impl> make_impl(toml::table table, std::filesystem::path source) {
    auto impl = std::make_unique<Config::Impl>();
    impl->table = std::move(table);
    impl->source = std::move(source);
    return impl;
}

}  // namespace

Config::Config() : impl_(std::make_unique<Impl>()) {}
Config::Config(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Config::Config(const Config& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}
Config::Config(Config&& other) noexcept : impl_(std::move(other.impl_)) {
    if (!other.impl_) other.impl_ = std::make_unique<Impl>();
}
Config& Config::operator=(const Config& other) {
    if (this != &other) *impl_ = *other.impl_;
    return *this;
}
Config& Config::operator=(Config&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
        other.impl_ = std::make_unique<Impl>();
    }
    return *this;
}
Config::~Config() = default;

Config Config::load_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw ConfigError("config file not found: " + path.string());
    }
    try {
        return Config(make_impl(toml::parse_file(path.string()), path));
    } catch (const toml::parse_error& err) {
        throw ConfigError(error_text(err, path.string()));
    }
}

Config Config::parse(std::string_view text, std::string_view source_name) {
    try {
        return Config(make_impl(toml::parse(text, source_name), std::filesystem::path(source_name)));
    } catch (const toml::parse_error& err) {
        throw ConfigError(error_text(err, source_name));
    }
}

bool Config::empty() const { return impl_->table.empty(); }

const std::filesystem::path& Config::source() const { return impl_->source; }

bool Config::contains(std::string_view path) const { return static_cast<bool>(impl_->table.at_path(path)); }

std::optional<nlohmann::json> Config::node(std::string_view path) const {
    auto view = impl_->table.at_path(path);
    if (!view) return std::nullopt;
    return toml_to_json(*view.node());
}

std::string Config::location(std::string_view path) const {
    auto view = impl_->table.at_path(path);
    if (!view) return {};
    const auto& src = view.node()->source();
    if (!src.begin.line) return {};  // set from code or an override, not read from a file
    std::ostringstream os;
    os << " at ";
    if (src.path) os << *src.path << ":";
    os << src.begin.line << ":" << src.begin.column;
    return os.str();
}

Config Config::section(std::string_view path) const {
    auto view = impl_->table.at_path(path);
    if (auto t = view.as_table()) return Config(make_impl(*t, impl_->source));
    return Config(make_impl({}, impl_->source));
}

std::vector<std::string> Config::keys() const {
    std::vector<std::string> out;
    out.reserve(impl_->table.size());
    for (const auto& [key, node] : impl_->table) out.emplace_back(key.str());
    return out;
}

void Config::set(std::string_view path, const nlohmann::json& value) {
    toml::table holder;
    try {
        holder = toml::parse("v = " + toml_literal(value));
    } catch (const toml::parse_error& err) {
        throw ConfigError("config key '" + std::string(path) + "': cannot store " + value.dump() + ": " +
                          std::string(err.description()));
    }
    const auto parts = split_dotted(path);
    toml::table* current = &impl_->table;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto* next = current->get(parts[i]);
        if (!next || !next->is_table()) {
            current->insert_or_assign(parts[i], toml::table{});
            next = current->get(parts[i]);
        }
        current = next->as_table();
    }
    auto* stored = holder.get("v");
    current->insert_or_assign(parts.back(), std::move(*stored));
}

void Config::apply_override(std::string_view assignment) {
    const auto eq = assignment.find('=');
    if (eq == std::string_view::npos) {
        throw ConfigError("invalid override '" + std::string(assignment) + "' (expected key=value)");
    }
    const auto key = trim(assignment.substr(0, eq));
    const auto raw = trim(assignment.substr(eq + 1));

    // A TOML value, or the bare text as a string.
    nlohmann::json value;
    try {
        const auto parsed = toml::parse("v = " + std::string(raw));
        value = toml_to_json(*parsed.get("v"));
    } catch (const toml::parse_error&) {
        value = std::string(raw);
    }
    set(key, value);
}

void Config::merge(const Config& other) { merge_tables(impl_->table, other.impl_->table); }

nlohmann::json Config::to_json() const { return toml_to_json(impl_->table); }

std::string Config::to_toml() const {
    std::ostringstream os;
    os << impl_->table;
    return os.str();
}

void Config::type_error(std::string_view path, std::string_view expected, const nlohmann::json& got) {
    throw ConfigError("config key '" + std::string(path) + "' must be " + std::string(expected) + " (got " +
                      got.type_name() + " " + got.dump() + ")");
}

}  // namespace forge
