#include "forge/config_schema.hpp"

#include "forge/config.hpp"

#include <format>

#include <algorithm>
#include <cmath>
#include <set>

namespace forge::config {
namespace {

std::string parent_of(const std::string& path) {
    const auto dot = path.rfind('.');
    return dot == std::string::npos ? std::string() : path.substr(0, dot);
}

bool type_matches(Type t, const json& v) {
    switch (t) {
        case Type::boolean: return v.is_boolean();
        case Type::integer: return v.is_number_integer() || v.is_number_unsigned();
        case Type::number: return v.is_number();
        case Type::string: return v.is_string();
        case Type::array: return v.is_array();
        case Type::table: return v.is_object();
        case Type::any: return true;
    }
    return true;
}

void leaves(const json& j, const std::string& prefix, std::vector<std::string>& out) {
    for (const auto& [k, v] : j.items()) {
        const std::string path = prefix.empty() ? k : prefix + "." + k;
        out.push_back(path);
        if (v.is_object()) leaves(v, path, out);
    }
}

}  // namespace

const char* to_string(Type t) {
    switch (t) {
        case Type::boolean: return "bool";
        case Type::integer: return "integer";
        case Type::number: return "number";
        case Type::string: return "string";
        case Type::array: return "array";
        case Type::table: return "table";
        case Type::any: return "any";
    }
    return "any";
}

Key& Key::doc(std::string text) {
    doc_text = std::move(text);
    return *this;
}
Key& Key::range(double lo, double hi) {
    min = lo;
    max = hi;
    return *this;
}
Key& Key::choices(std::vector<std::string> values) {
    choice_values = std::move(values);
    return *this;
}
Key& Key::unit(std::string u) {
    unit_text = std::move(u);
    return *this;
}
Key& Key::required(bool r) {
    is_required = r;
    return *this;
}
Key& Key::computed(bool c) {
    is_computed = c;
    return *this;
}

Key& Key::secret(bool s) {
    is_secret = s;
    return *this;
}

Schema& Schema::section(const std::string& path, std::string doc) {
    auto& s = sections_[path];
    if (!doc.empty()) s.doc = std::move(doc);
    return *this;
}

Schema& Schema::open_section(const std::string& path, std::string doc) {
    auto& s = sections_[path];
    s.open = true;
    if (!doc.empty()) s.doc = std::move(doc);
    return *this;
}

Key& Schema::key(const std::string& path, Type type, json def, std::string doc) {
    auto& k = keys_[path];
    k.path = path;
    k.type = type;
    k.default_value = std::move(def);
    if (!doc.empty()) k.doc_text = std::move(doc);
    // Declaring a key declares its section (so unknown siblings are reported).
    for (auto p = parent_of(path); !p.empty(); p = parent_of(p)) sections_.try_emplace(p);
    return k;
}

Key& Schema::boolean(const std::string& p, bool d, std::string doc) { return key(p, Type::boolean, d, std::move(doc)); }
Key& Schema::integer(const std::string& p, std::int64_t d, std::string doc) {
    return key(p, Type::integer, d, std::move(doc));
}
Key& Schema::number(const std::string& p, double d, std::string doc) { return key(p, Type::number, d, std::move(doc)); }
Key& Schema::string(const std::string& p, std::string d, std::string doc) {
    return key(p, Type::string, std::move(d), std::move(doc));
}
Key& Schema::array(const std::string& p, json d, std::string doc) {
    return key(p, Type::array, std::move(d), std::move(doc));
}
Key& Schema::table(const std::string& p, std::string doc) {
    open_section(p);
    return key(p, Type::table, json::object(), std::move(doc));
}

const Key* Schema::find(const std::string& path) const {
    auto it = keys_.find(path);
    return it == keys_.end() ? nullptr : &it->second;
}

bool Schema::covered_by_open_section(const std::string& path) const {
    for (auto p = parent_of(path); !p.empty(); p = parent_of(p)) {
        auto it = sections_.find(p);
        if (it != sections_.end() && it->second.open) return true;
    }
    return false;
}

Schema& Schema::alias(const std::string& old_path, const std::string& path) {
    aliases_[old_path] = path;
    return *this;
}

std::string Schema::aliased(const std::string& path) const {
    const auto it = aliases_.find(path);
    return it == aliases_.end() ? std::string() : it->second;
}

Schema::Result Schema::validate(const Config& cfg) const {
    Result r;
    const json all = cfg.to_json();
    for (const auto& [old_path, path] : aliases_) {
        if (!cfg.contains(old_path)) continue;
        if (cfg.contains(path) && cfg.get<json>(path) != cfg.get<json>(old_path)) {
            r.errors.push_back(std::format("{}: set together with {} (its new name) to another value", old_path, path));
        } else {
            r.warnings.push_back(std::format("{}: deprecated, use {}", old_path, path));
        }
    }
    for (const auto& [path, k] : keys_) {
        const auto v = cfg.get<json>(path);
        if (!v) {
            if (k.is_required) r.errors.push_back(std::format("{}: required ({})", path, to_string(k.type)));
            continue;
        }
        if (!type_matches(k.type, *v)) {
            r.errors.push_back(std::format("{}: expected {}, got {}", path, to_string(k.type), v->dump()));
            continue;
        }
        if (v->is_number() && (k.min || k.max)) {
            const double d = v->get<double>();
            if ((k.min && d < *k.min) || (k.max && d > *k.max) || std::isnan(d)) {
                r.errors.push_back(std::format(
                    "{}: {} is outside {} .. {}{}", path, v->dump(), k.min ? std::format("{}", *k.min) : "-inf",
                    k.max ? std::format("{}", *k.max) : "+inf", k.unit_text.empty() ? "" : " " + k.unit_text));
            }
        }
        if (v->is_string() && !k.choice_values.empty() &&
            std::find(k.choice_values.begin(), k.choice_values.end(), v->get<std::string>()) == k.choice_values.end()) {
            std::string list;
            for (const auto& c : k.choice_values) list += (list.empty() ? "" : " | ") + c;
            r.errors.push_back(std::format("{}: \"{}\" is not one of {}", path, v->get<std::string>(), list));
        }
    }
    // Unknown keys inside declared sections: probably typos.
    std::vector<std::string> present;
    leaves(all, "", present);
    for (const auto& path : present) {
        if (keys_.count(path) || sections_.count(path) || aliases_.count(path) || covered_by_open_section(path)) {
            continue;
        }
        const auto parent = parent_of(path);
        if (parent.empty() || !sections_.count(parent)) continue;  // section nobody declared
        // Suggest the closest declared key of that section.
        std::string best;
        std::size_t best_d = 4;
        for (const auto& [kp, k] : keys_) {
            if (parent_of(kp) != parent) continue;
            const auto& a = path;
            const auto& b = kp;
            std::vector<std::size_t> d(b.size() + 1);
            for (std::size_t j = 0; j <= b.size(); ++j) d[j] = j;
            for (std::size_t i = 1; i <= a.size(); ++i) {
                std::size_t prev = d[0];
                d[0] = i;
                for (std::size_t j = 1; j <= b.size(); ++j) {
                    const std::size_t cur = d[j];
                    d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
                    prev = cur;
                }
            }
            if (d[b.size()] < best_d) {
                best_d = d[b.size()];
                best = kp;
            }
        }
        r.warnings.push_back(best.empty() ? std::format("{}: unknown key", path)
                                          : std::format("{}: unknown key (did you mean {}?)", path, best));
    }
    return r;
}

void Schema::apply_defaults(Config& cfg) const {
    // Old names first: their value becomes the new key's, which then needs no default.
    for (const auto& [old_path, path] : aliases_) {
        if (!cfg.contains(old_path) || cfg.contains(path)) continue;
        if (auto v = cfg.get<json>(old_path); v && v->is_primitive() && !v->is_null()) {
            cfg.apply_override(path + "=" + v->dump());
        }
    }
    for (const auto& [path, k] : keys_) {
        if (k.type == Type::table || k.type == Type::any || k.default_value.is_null() || k.is_computed) continue;
        if (cfg.contains(path)) continue;
        if (k.default_value.is_array()) {
            const bool scalars = std::all_of(k.default_value.begin(), k.default_value.end(),
                                             [](const json& v) { return v.is_primitive() && !v.is_null(); });
            if (!scalars) continue;
        } else if (!k.default_value.is_primitive()) {
            continue;
        }
        // Leave it to validation when a parent is not a table (e.g. `http = 5`).
        bool parents_ok = true;
        for (auto p = parent_of(path); !p.empty(); p = parent_of(p)) {
            if (auto v = cfg.get<json>(p); v && !v->is_object()) {
                parents_ok = false;
                break;
            }
        }
        if (!parents_ok) continue;
        // A JSON scalar or array of scalars is a valid TOML value.
        cfg.apply_override(path + "=" + k.default_value.dump());
    }
}

std::string Schema::help() const {
    std::string deprecated;
    for (const auto& [old_path, path] : aliases_) {
        deprecated += std::format("# {} = ...   # deprecated: use {}\n", old_path, path);
    }
    std::map<std::string, std::vector<const Key*>> by_section;
    for (const auto& [p, k] : keys_) by_section[parent_of(p)].push_back(&k);
    std::set<std::string> names;
    for (const auto& [p, s] : sections_) names.insert(p);
    for (const auto& [p, v] : by_section) names.insert(p);

    std::string out;
    for (const auto& sec : names) {
        auto sit = sections_.find(sec);
        auto kit = by_section.find(sec);
        const bool has_keys = kit != by_section.end();
        const bool has_doc = sit != sections_.end() && !sit->second.doc.empty();
        if (!has_keys && !has_doc) continue;
        out += sec.empty() ? "\n# (top level)\n" : std::format("\n[{}]\n", sec);
        if (has_doc) out += std::format("# {}\n", sit->second.doc);
        if (sit != sections_.end() && sit->second.open) out += "# (free-form keys)\n";
        if (!has_keys) continue;
        for (const Key* k : kit->second) {
            const auto name = k->path.substr(sec.empty() ? 0 : sec.size() + 1);
            std::string def = k->is_secret ? "\"...\"" : k->default_value.dump();
            if (k->type == Type::table) def = "{}";
            std::string note = to_string(k->type);
            if (k->min || k->max) {
                note += std::format(", {} .. {}", k->min ? std::format("{}", *k->min) : "",
                                    k->max ? std::format("{}", *k->max) : "");
            }
            if (!k->unit_text.empty()) note += " " + k->unit_text;
            if (!k->choice_values.empty()) {
                std::string list;
                for (const auto& c : k->choice_values) list += (list.empty() ? "" : "|") + c;
                note += ": " + list;
            }
            if (k->is_required) note += ", required";
            out += std::format("{:<36} # {}{}\n", name + " = " + def, k->doc_text.empty() ? "" : k->doc_text + " ",
                               "(" + note + ")");
        }
    }
    if (!deprecated.empty()) out += "\n# Deprecated keys (still accepted)\n" + deprecated;
    return out;
}

}  // namespace forge::config
