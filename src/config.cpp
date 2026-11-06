#include "forge/config.hpp"

#include <sstream>

namespace forge {
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
    os << source << ":" << err.source().begin.line << ":" << err.source().begin.column << ": "
       << err.description();
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

}  // namespace

Config::Config(toml::table table, std::filesystem::path source)
    : table_(std::move(table)), source_(std::move(source)) {}

Config Config::load_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw ConfigError("config file not found: " + path.string());
    }
    try {
        return Config(toml::parse_file(path.string()), path);
    } catch (const toml::parse_error& err) {
        throw ConfigError(error_text(err, path.string()));
    }
}

Config Config::parse(std::string_view text, std::string_view source_name) {
    try {
        return Config(toml::parse(text, source_name), std::filesystem::path(source_name));
    } catch (const toml::parse_error& err) {
        throw ConfigError(error_text(err, source_name));
    }
}

bool Config::contains(std::string_view path) const { return static_cast<bool>(table_.at_path(path)); }

Config Config::section(std::string_view path) const {
    auto view = table_.at_path(path);
    if (auto t = view.as_table()) return Config(*t, source_);
    return Config({}, source_);
}

std::vector<std::string> Config::keys() const {
    std::vector<std::string> out;
    out.reserve(table_.size());
    for (const auto& [key, node] : table_) out.emplace_back(key.str());
    return out;
}

void Config::set_node(std::string_view path, toml::table holder) {
    const auto parts = split_dotted(path);
    toml::table* current = &table_;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto* next = current->get(parts[i]);
        if (!next || !next->is_table()) {
            current->insert_or_assign(parts[i], toml::table{});
            next = current->get(parts[i]);
        }
        current = next->as_table();
    }
    auto* value = holder.get("v");
    current->insert_or_assign(parts.back(), std::move(*value));
}

void Config::apply_override(std::string_view assignment) {
    const auto eq = assignment.find('=');
    if (eq == std::string_view::npos) {
        throw ConfigError("invalid override '" + std::string(assignment) + "' (expected key=value)");
    }
    const auto key = trim(assignment.substr(0, eq));
    const auto raw = trim(assignment.substr(eq + 1));

    toml::table holder;
    try {
        holder = toml::parse("v = " + std::string(raw));
    } catch (const toml::parse_error&) {
        holder = toml::table{};
        holder.insert_or_assign("v", std::string(raw));
    }
    set_node(key, std::move(holder));
}

void Config::merge(const Config& other) { merge_tables(table_, other.table_); }

nlohmann::json toml_to_json(const toml::node& node) {
    std::ostringstream os;
    if (auto t = node.as_table()) {
        os << toml::json_formatter{*t};
    } else if (auto a = node.as_array()) {
        os << toml::json_formatter{*a};
    } else {
        // Wrap scalars in a table so the formatter accepts them.
        toml::table wrapper;
        wrapper.insert_or_assign("v", node);
        os << toml::json_formatter{wrapper};
        return nlohmann::json::parse(os.str()).at("v");
    }
    return nlohmann::json::parse(os.str());
}

nlohmann::json Config::to_json() const { return toml_to_json(table_); }

std::string Config::to_toml() const {
    std::ostringstream os;
    os << table_;
    return os.str();
}

void Config::type_error(std::string_view path, std::string_view expected, const toml::node& node) {
    std::ostringstream os;
    os << "config key '" << path << "' must be " << expected << " (got " << node.type() << ")";
    if (const auto& src = node.source(); src.begin.line) {
        os << " at ";
        if (src.path) os << *src.path << ":";
        os << src.begin.line << ":" << src.begin.column;
    }
    throw ConfigError(os.str());
}

}  // namespace forge
