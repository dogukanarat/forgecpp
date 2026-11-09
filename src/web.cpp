#include "forge/web.hpp"

#include "forge/http.hpp"
#include "forge/log.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <stdexcept>

namespace forge::web {

namespace generated {
// Produced by ForgeEmbed.cmake from src/web/ at library build time.
const Asset* forgecpp_builtin_assets(std::size_t& count);
}  // namespace generated

// ---------------------------------------------------------------------------
// asset registry

namespace {
std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Asset>& registry() {
    static std::vector<Asset> r;
    return r;
}
}  // namespace

void register_assets(const Asset* assets, std::size_t count) {
    std::lock_guard lock(registry_mutex());
    for (std::size_t i = 0; i < count; ++i) {
        if (assets[i].path && assets[i].path[0] == '/') registry().push_back(assets[i]);
    }
}

std::vector<Asset> registered_assets() {
    std::lock_guard lock(registry_mutex());
    return registry();
}

std::vector<Asset> builtin_assets() {
    std::size_t count = 0;
    const Asset* assets = generated::forgecpp_builtin_assets(count);
    return {assets, assets + count};
}

std::string mime_type(std::string_view path) {
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos) return "application/octet-stream";
    std::string ext(path.substr(dot + 1));
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    static const std::map<std::string, std::string, std::less<>> types{
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json"},
        {"map", "application/json"},
        {"txt", "text/plain; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},
        {"xml", "application/xml"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"ico", "image/x-icon"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"otf", "font/otf"},
        {"wasm", "application/wasm"},
        {"pdf", "application/pdf"},
        {"webmanifest", "application/manifest+json"},
    };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

// ---------------------------------------------------------------------------
// Control

const char* to_string(Kind kind) {
    switch (kind) {
        case Kind::button: return "button";
        case Kind::toggle: return "toggle";
        case Kind::slider: return "slider";
        case Kind::number: return "number";
        case Kind::text: return "text";
        case Kind::select: return "select";
        case Kind::value: return "value";
    }
    return "value";
}

Control& Control::unit(std::string u) {
    def_->unit = std::move(u);
    return *this;
}
Control& Control::help(std::string text) {
    def_->help = std::move(text);
    return *this;
}
Control& Control::confirm(std::string question) {
    def_->confirm = std::move(question);
    return *this;
}
Control& Control::precision(int digits) {
    def_->precision = std::max(0, digits);
    return *this;
}
Control& Control::range(double min, double max, double step) {
    if (min > max) throw std::invalid_argument("control '" + def_->id + "': min > max");
    def_->min = min;
    def_->max = max;
    if (step > 0) def_->step = step;
    return *this;
}
Control& Control::readonly(bool value) {
    def_->readonly = value;
    return *this;
}
Control& Control::role(Role r) {
    def_->role = r;
    return *this;
}

Control& Control::persist(bool on) {
    def_->persist = on;
    return *this;
}

CommandRef& CommandRef::help(std::string text) {
    def_->help = std::move(text);
    return *this;
}
CommandRef& CommandRef::confirm(std::string question) {
    def_->confirm = std::move(question);
    return *this;
}
CommandRef& CommandRef::role(Role r) {
    def_->role = r;
    return *this;
}

Param number_param(std::string name, std::optional<double> def, std::string help) {
    Param p{std::move(name), "number", std::move(help), def ? json(*def) : json(nullptr), {}, {}, {}};
    return p;
}
Param integer_param(std::string name, std::optional<std::int64_t> def, std::string help) {
    return Param{std::move(name), "integer", std::move(help), def ? json(*def) : json(nullptr), {}, {}, {}};
}
Param bool_param(std::string name, std::optional<bool> def, std::string help) {
    return Param{std::move(name), "bool", std::move(help), def ? json(*def) : json(nullptr), {}, {}, {}};
}
Param text_param(std::string name, std::optional<std::string> def, std::string help) {
    return Param{std::move(name), "text", std::move(help), def ? json(*def) : json(nullptr), {}, {}, {}};
}
Param select_param(std::string name, std::vector<std::string> options, std::optional<std::string> def,
                   std::string help) {
    return Param{
        std::move(name), "select", std::move(help), def ? json(*def) : json(nullptr), std::move(options), {}, {}};
}

CommandRef Group::command(const std::string& id, const std::string& label, std::vector<Param> params,
                          std::function<json(const json&)> handler) {
    if (owner_.commands_by_id_.count(id) || owner_.by_id_.count(id)) {
        throw std::invalid_argument("duplicate dashboard id '" + id + "'");
    }
    auto def = std::make_unique<detail::CommandDef>();
    def->id = id;
    def->label = label;
    def->group = title_;
    def->params = std::move(params);
    def->handler = std::move(handler);
    auto* stored = def.get();
    owner_.commands_.push_back(std::move(def));
    owner_.commands_by_id_[id] = stored;
    command_ids_.push_back(id);
    return CommandRef(stored);
}

Control& Control::primary() {
    def_->style = "primary";
    return *this;
}
Control& Control::danger() {
    def_->style = "danger";
    return *this;
}

// ---------------------------------------------------------------------------
// Group

Control Group::add(Kind kind, const std::string& id, const std::string& label, Getter get, Setter set) {
    detail::ControlDef def;
    def.id = id;
    def.kind = kind;
    def.label = label.empty() ? id : label;
    def.group = title_;
    def.get = std::move(get);
    def.set = std::move(set);
    auto& stored = owner_.add(std::move(def));
    ids_.push_back(id);
    return Control(&stored);
}

Control Group::button(const std::string& id, const std::string& label, std::function<void()> action) {
    auto c = add(Kind::button, id, label, {}, {});
    owner_.by_id_.at(id)->action = std::move(action);
    return c;
}

Control Group::toggle(const std::string& id, const std::string& label, bool& value) {
    return add(
        Kind::toggle, id, label, [&value] { return json(value); }, [&value](const json& v) { value = v.get<bool>(); });
}

Control Group::toggle(const std::string& id, const std::string& label, std::function<bool()> get,
                      std::function<void(bool)> set) {
    return add(
        Kind::toggle, id, label, [get = std::move(get)] { return json(get()); },
        [set = std::move(set)](const json& v) { set(v.get<bool>()); });
}

Control Group::slider(const std::string& id, const std::string& label, std::function<double()> get,
                      std::function<void(double)> set, double min, double max, double step) {
    return add(
               Kind::slider, id, label, [get = std::move(get)] { return json(get()); },
               [set = std::move(set)](const json& v) { set(v.get<double>()); })
        .range(min, max, step);
}

Control Group::number(const std::string& id, const std::string& label, std::function<double()> get,
                      std::function<void(double)> set) {
    return add(
        Kind::number, id, label, [get = std::move(get)] { return json(get()); },
        [set = std::move(set)](const json& v) { set(v.get<double>()); });
}

Control Group::text(const std::string& id, const std::string& label, std::string& value) {
    return add(
        Kind::text, id, label, [&value] { return json(value); },
        [&value](const json& v) { value = v.get<std::string>(); });
}

Control Group::text(const std::string& id, const std::string& label, std::function<std::string()> get,
                    std::function<void(const std::string&)> set) {
    return add(
        Kind::text, id, label, [get = std::move(get)] { return json(get()); },
        [set = std::move(set)](const json& v) { set(v.get<std::string>()); });
}

Control Group::select(const std::string& id, const std::string& label, std::vector<std::string> options,
                      std::string& value) {
    auto c = add(
        Kind::select, id, label, [&value] { return json(value); },
        [&value](const json& v) { value = v.get<std::string>(); });
    owner_.by_id_.at(id)->options = std::move(options);
    return c;
}

Control Group::select(const std::string& id, const std::string& label, std::vector<std::string> options,
                      std::function<std::string()> get, std::function<void(const std::string&)> set) {
    auto c = add(
        Kind::select, id, label, [get = std::move(get)] { return json(get()); },
        [set = std::move(set)](const json& v) { set(v.get<std::string>()); });
    owner_.by_id_.at(id)->options = std::move(options);
    return c;
}

Control Group::value_getter(const std::string& id, const std::string& label, Getter get) {
    return add(Kind::value, id, label, std::move(get), {}).readonly();
}

// ---------------------------------------------------------------------------
// Dashboard

Dashboard::Dashboard(std::string title) : title_(std::move(title)) {}
Dashboard::~Dashboard() = default;

Group& Dashboard::group(const std::string& title) {
    for (auto& g : groups_) {
        if (g->title() == title) return *g;
    }
    groups_.push_back(std::unique_ptr<Group>(new Group(*this, title)));
    return *groups_.back();
}

void Dashboard::link(const std::string& label, const std::string& href) { links_.emplace_back(label, href); }

detail::ControlDef& Dashboard::add(detail::ControlDef def) {
    if (def.id.empty()) throw std::invalid_argument("dashboard control id must not be empty");
    for (char c : def.id) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) {
            throw std::invalid_argument("dashboard control id '" + def.id +
                                        "' may only contain letters, digits, '_', '-' and '.'");
        }
    }
    if (by_id_.count(def.id)) throw std::invalid_argument("duplicate dashboard control id '" + def.id + "'");
    controls_.push_back(std::make_unique<detail::ControlDef>(std::move(def)));
    auto* stored = controls_.back().get();
    by_id_[stored->id] = stored;
    return *stored;
}

bool Dashboard::contains(const std::string& id) const { return by_id_.count(id) > 0; }

std::vector<std::string> Dashboard::ids() const {
    std::vector<std::string> out;
    for (const auto& c : controls_) out.push_back(c->id);
    return out;
}

const detail::ControlDef& Dashboard::control(const std::string& id) const {
    auto it = by_id_.find(id);
    if (it == by_id_.end()) throw forge::Error(404, "unknown control '" + id + "'");
    return *it->second;
}

namespace {
json read_value(const detail::ControlDef& c) {
    if (!c.get) return nullptr;
    try {
        return c.get();
    } catch (const std::exception& e) {
        log::warn("dashboard: reading '{}' failed: {}", c.id, e.what());
        return nullptr;
    }
}
}  // namespace

void Dashboard::check_thread(const char* method) const {
    if (owner_ == std::thread::id{} || std::this_thread::get_id() == owner_) return;
    throw std::logic_error(std::string("web::Dashboard::") + method +
                           "() called outside the main loop: the controls belong to the application's main loop; "
                           "use app.invoke([&] { ... }) or app.post(...) from other threads");
}

json Dashboard::schema() const {
    check_thread("schema");
    json groups = json::array();
    for (const auto& g : groups_) {
        json controls = json::array();
        for (const auto& id : g->ids_) {
            const auto& c = *by_id_.at(id);
            json j{{"id", c.id}, {"kind", to_string(c.kind)}, {"label", c.label}};
            if (!c.unit.empty()) j["unit"] = c.unit;
            if (!c.help.empty()) j["help"] = c.help;
            if (!c.confirm.empty()) j["confirm"] = c.confirm;
            if (!c.style.empty()) j["style"] = c.style;
            if (c.min) j["min"] = *c.min;
            if (c.max) j["max"] = *c.max;
            if (c.step) j["step"] = *c.step;
            if (c.precision) j["precision"] = *c.precision;
            if (!c.options.empty()) j["options"] = c.options;
            if (c.readonly) j["readonly"] = true;
            if (c.role != Role::operator_) j["role"] = to_string(c.role);
            if (c.persist) j["persist"] = true;
            if (c.kind != Kind::button) j["value"] = read_value(c);
            controls.push_back(std::move(j));
        }
        json commands = json::array();
        for (const auto& id : g->command_ids_) {
            const auto& c = *commands_by_id_.at(id);
            json params = json::array();
            for (const auto& p : c.params) {
                json pj{{"name", p.name}, {"type", p.type}};
                if (!p.help.empty()) pj["help"] = p.help;
                if (!p.default_value.is_null()) pj["default"] = p.default_value;
                if (!p.options.empty()) pj["options"] = p.options;
                if (p.min) pj["min"] = *p.min;
                if (p.max) pj["max"] = *p.max;
                params.push_back(std::move(pj));
            }
            json cj{{"id", c.id}, {"label", c.label}, {"params", std::move(params)}};
            if (!c.help.empty()) cj["help"] = c.help;
            if (!c.confirm.empty()) cj["confirm"] = c.confirm;
            if (c.role != Role::operator_) cj["role"] = to_string(c.role);
            commands.push_back(std::move(cj));
        }
        json gj{{"title", g->title()}, {"controls", std::move(controls)}};
        if (!commands.empty()) gj["commands"] = std::move(commands);
        groups.push_back(std::move(gj));
    }
    json links = json::array();
    for (const auto& [label, href] : links_) links.push_back({{"label", label}, {"href", href}});
    return {{"title", title_}, {"groups", std::move(groups)}, {"links", std::move(links)}};
}

json Dashboard::state() const {
    check_thread("state");
    json out = json::object();
    for (const auto& c : controls_) {
        if (c->kind != Kind::button) out[c->id] = read_value(*c);
    }
    return out;
}

json Dashboard::get(const std::string& id) const {
    check_thread("get");
    const auto& c = control(id);
    return c.kind == Kind::button ? json(nullptr) : read_value(c);
}

void Dashboard::press(const std::string& id) {
    check_thread("press");
    const auto& c = control(id);
    if (c.kind != Kind::button) throw forge::Error(400, "control '" + id + "' is not a button");
    if (c.action) c.action();
}

void Dashboard::set(const std::string& id, const json& value) {
    check_thread("set");
    auto it = by_id_.find(id);
    if (it == by_id_.end()) throw forge::Error(404, "unknown control '" + id + "'");
    auto& c = *it->second;

    if (c.kind == Kind::button) {
        press(id);
        return;
    }
    if (c.readonly || !c.set) throw forge::Error(403, "control '" + id + "' is read-only");

    auto bad = [&](const std::string& why) { return forge::Error(400, "control '" + id + "': " + why); };
    switch (c.kind) {
        case Kind::toggle:
            if (!value.is_boolean()) throw bad("expected true or false");
            break;
        case Kind::slider:
        case Kind::number: {
            if (!value.is_number()) throw bad("expected a number");
            const double d = value.get<double>();
            if (!std::isfinite(d)) throw bad("expected a finite number");
            if (c.min && d < *c.min) throw bad("must be >= " + json(*c.min).dump());
            if (c.max && d > *c.max) throw bad("must be <= " + json(*c.max).dump());
            break;
        }
        case Kind::text:
            if (!value.is_string()) throw bad("expected a string");
            break;
        case Kind::select:
            if (!value.is_string()) throw bad("expected a string");
            if (std::find(c.options.begin(), c.options.end(), value.get<std::string>()) == c.options.end()) {
                throw bad("must be one of " + json(c.options).dump());
            }
            break;
        default: throw bad("cannot be set");
    }
    c.set(value);
}

bool Dashboard::has_command(const std::string& id) const { return commands_by_id_.count(id) > 0; }

std::vector<std::string> Dashboard::command_ids() const {
    std::vector<std::string> out;
    for (const auto& c : commands_) out.push_back(c->id);
    return out;
}

const detail::CommandDef& Dashboard::command(const std::string& id) const {
    auto it = commands_by_id_.find(id);
    if (it == commands_by_id_.end()) throw forge::Error(404, "unknown command '" + id + "'");
    return *it->second;
}

json Dashboard::call(const std::string& id, const json& args) {
    check_thread("call");
    const auto& c = command(id);
    if (!args.is_null() && !args.is_object()) throw forge::Error(400, "arguments must be an object");
    json in = args.is_object() ? args : json::object();
    json out = json::object();
    auto bad = [&](const std::string& p, const std::string& why) {
        return forge::Error(400, "command '" + id + "': " + p + ": " + why);
    };
    for (const auto& p : c.params) {
        json v = in.contains(p.name) ? in[p.name] : p.default_value;
        in.erase(p.name);
        if (v.is_null()) throw bad(p.name, "required");
        if (p.type == "number" || p.type == "integer") {
            if (v.is_string()) {  // console / query strings
                auto parsed = json::parse(v.get<std::string>(), nullptr, false);
                if (!parsed.is_discarded()) v = std::move(parsed);
            }
            if (!v.is_number()) throw bad(p.name, "expected a number");
            const double d = v.get<double>();
            if (p.type == "integer" && d != std::floor(d)) throw bad(p.name, "expected an integer");
            if (p.min && d < *p.min) throw bad(p.name, "must be >= " + json(*p.min).dump());
            if (p.max && d > *p.max) throw bad(p.name, "must be <= " + json(*p.max).dump());
            if (p.type == "integer") v = static_cast<std::int64_t>(d);
        } else if (p.type == "bool") {
            if (v.is_string()) v = (v == "true" || v == "1" || v == "yes" || v == "on");
            if (!v.is_boolean()) throw bad(p.name, "expected true or false");
        } else {
            if (!v.is_string()) v = v.dump();
            if (p.type == "select" &&
                std::find(p.options.begin(), p.options.end(), v.get<std::string>()) == p.options.end()) {
                throw bad(p.name, "must be one of " + json(p.options).dump());
            }
        }
        out[p.name] = v;
    }
    if (!in.empty()) throw bad(in.begin().key(), "unknown parameter");
    return c.handler ? c.handler(out) : json(nullptr);
}

std::vector<std::string> Dashboard::persistent_ids() const {
    std::vector<std::string> out;
    for (const auto& c : controls_) {
        if (c->persist) out.push_back(c->id);
    }
    return out;
}

json Dashboard::changes() {
    check_thread("changes");
    json current = state();
    json diff = json::object();
    for (auto& [id, value] : current.items()) {
        auto it = last_state_.find(id);
        if (it == last_state_.end() || *it != value) diff[id] = value;
    }
    last_state_ = std::move(current);
    return diff;
}

}  // namespace forge::web
