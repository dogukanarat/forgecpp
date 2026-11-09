#pragma once
// Web control panel.
//
// Declare controls once; forgecpp then gives you, with no HTML or JS to write:
//   * a live dashboard at /forge/ (groups, buttons, switches, sliders, values, logs)
//   * a JSON API at /forge/api/... and live updates over Server-Sent Events
//   * forge.js for your own pages: <input data-forge-bind="speed"> just works
//   * console commands `controls`, `set <id> <value>`, `press <id>` as a backup
//
//   void setup_web(forge::web::Dashboard& ui) override {
//       auto& motor = ui.group("Motor");
//       motor.button("start", "Start", [this] { start(); });
//       motor.button("stop", "Stop", [this] { stop_motor(); }).confirm("Stop the motor?").danger();
//       motor.toggle("enabled", "Enabled", enabled_);
//       motor.slider("speed", "Speed", speed_, 0, 3000, 10).unit("rpm");
//       motor.value("temperature", "Temperature", temperature_).unit("°C").precision(1);
//       motor.select("mode", "Mode", {"auto", "manual"}, mode_);
//       ui.link("Wiring diagram", "/wiring.html");
//   }
//
// Bound variables are read and written on the application main loop, so they
// need no locking. Every control is addressed by a unique id.

#include "forge/assets.hpp"
#include "forge/auth.hpp"
#include "forge/metrics.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace forge::web {

using json = nlohmann::json;
using Getter = std::function<json()>;
using Setter = std::function<void(const json&)>;

enum class Kind { button, toggle, slider, number, text, select, value };

const char* to_string(Kind kind);

namespace detail {
struct ControlDef {
    std::string id;
    Kind kind = Kind::value;
    std::string label;
    std::string group;
    std::string unit;
    std::string help;
    std::string confirm;
    std::string style;  ///< "", "primary", "danger"
    std::optional<double> min, max, step;
    std::optional<int> precision;
    std::vector<std::string> options;
    bool readonly = false;
    Role role = Role::operator_;  ///< needed to change it
    bool persist = false;         ///< saved to [state].file and restored at start
    Getter get;
    Setter set;
    std::function<void()> action;
};
}  // namespace detail

/// Parameter of a command (see Group::command).
struct Param {
    std::string name;
    std::string type = "number";  ///< number | integer | bool | text | select
    std::string help;
    json default_value;           ///< null: required
    std::vector<std::string> options;
    std::optional<double> min, max;

    Param& describe(std::string text) {
        help = std::move(text);
        return *this;
    }
    Param& range(double lo, double hi) {
        min = lo;
        max = hi;
        return *this;
    }
};
Param number_param(std::string name, std::optional<double> def = std::nullopt, std::string help = {});
Param integer_param(std::string name, std::optional<std::int64_t> def = std::nullopt, std::string help = {});
Param bool_param(std::string name, std::optional<bool> def = std::nullopt, std::string help = {});
Param text_param(std::string name, std::optional<std::string> def = std::nullopt, std::string help = {});
Param select_param(std::string name, std::vector<std::string> options, std::optional<std::string> def = std::nullopt,
                   std::string help = {});

namespace detail {
struct CommandDef {
    std::string id, label, help, group, confirm;
    std::vector<Param> params;
    Role role = Role::operator_;
    std::function<json(const json& args)> handler;
};
}  // namespace detail

/// Fluent options for a command.
class CommandRef {
public:
    CommandRef& help(std::string text);
    CommandRef& confirm(std::string question);
    CommandRef& role(Role r);
    const std::string& id() const { return def_->id; }

private:
    friend class Group;
    explicit CommandRef(detail::CommandDef* def) : def_(def) {}
    detail::CommandDef* def_;
};

/// Fluent options for a control.
class Control {
public:
    Control& unit(std::string unit);
    Control& help(std::string text);
    /// Ask for confirmation in the browser before applying.
    Control& confirm(std::string question);
    /// Decimal places shown for numbers.
    Control& precision(int digits);
    /// Allowed range (validated server-side) and step for number/slider.
    Control& range(double min, double max, double step = 0);
    Control& readonly(bool value = true);
    Control& primary();
    Control& danger();
    /// Role needed to change it (default: operator).
    Control& role(Role r);
    /// Save the value to [state].file on change and restore it at start-up
    /// (before on_start(); the setter must work without on_start()'s resources).
    Control& persist(bool on = true);

    const std::string& id() const { return def_->id; }

private:
    friend class Group;
    explicit Control(detail::ControlDef* def) : def_(def) {}
    detail::ControlDef* def_;
};

class Dashboard;

class Group {
public:
    const std::string& title() const { return title_; }

    /// Push button running `action`.
    Control button(const std::string& id, const std::string& label, std::function<void()> action);

    /// Command with parameters (RPC): the panel shows a form, the console
    /// takes `call <id> name=value ...`, forgectl and links the "call" command. Arguments
    /// are validated (types, ranges, choices, defaults) before `handler` runs
    /// on the main loop; its JSON result goes back to the caller.
    CommandRef command(const std::string& id, const std::string& label, std::vector<Param> params,
                       std::function<json(const json& args)> handler);

    /// On/off switch.
    Control toggle(const std::string& id, const std::string& label, bool& value);
    Control toggle(const std::string& id, const std::string& label, std::function<bool()> get,
                   std::function<void(bool)> set);

    /// Slider between min and max.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Control slider(const std::string& id, const std::string& label, T& value, double min, double max, double step = 1) {
        return numeric(Kind::slider, id, label, value).range(min, max, step);
    }
    Control slider(const std::string& id, const std::string& label, std::function<double()> get,
                   std::function<void(double)> set, double min, double max, double step = 1);

    /// Numeric input. Use .range() to restrict it.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Control number(const std::string& id, const std::string& label, T& value) {
        return numeric(Kind::number, id, label, value);
    }
    Control number(const std::string& id, const std::string& label, std::function<double()> get,
                   std::function<void(double)> set);

    /// Text input.
    Control text(const std::string& id, const std::string& label, std::string& value);
    Control text(const std::string& id, const std::string& label, std::function<std::string()> get,
                 std::function<void(const std::string&)> set);

    /// Drop-down list.
    Control select(const std::string& id, const std::string& label, std::vector<std::string> options,
                   std::string& value);
    Control select(const std::string& id, const std::string& label, std::vector<std::string> options,
                   std::function<std::string()> get, std::function<void(const std::string&)> set);

    /// Read-only value bound to a variable (anything convertible to JSON).
    template <class T, class = std::enable_if_t<!std::is_invocable_v<const T&>>>
    Control value(const std::string& id, const std::string& label, const T& ref) {
        return value(id, label, Getter([&ref] { return json(ref); }));
    }
    /// Read-only value of an atomic another thread updates (a counter, a flag).
    template <class T>
    Control value(const std::string& id, const std::string& label, const std::atomic<T>& ref) {
        return value(id, label, [&ref] { return ref.load(std::memory_order_relaxed); });
    }
    /// A counter or gauge of the metrics registry: the panel shows what /metrics shows.
    Control value(const std::string& id, const std::string& label, const metrics::Counter& c) {
        return value(id, label, [&c] { return c.value(); });
    }
    Control value(const std::string& id, const std::string& label, const metrics::Gauge& g) {
        return value(id, label, [&g] { return g.value(); });
    }
    /// Read-only value computed by `get` (its result is converted to JSON).
    template <class F, class = std::enable_if_t<std::is_invocable_v<const F&>>, class = void>
    Control value(const std::string& id, const std::string& label, F get) {
        return value_getter(id, label, Getter([get = std::move(get)] { return json(get()); }));
    }

private:
    friend class Dashboard;
    Group(Dashboard& owner, std::string title) : owner_(owner), title_(std::move(title)) {}

    template <class T>
    Control numeric(Kind kind, const std::string& id, const std::string& label, T& value) {
        return add(kind, id, label, [&value] { return json(value); }, [&value](const json& v) {
            const double d = v.get<double>();
            if constexpr (std::is_integral_v<T>) {
                value = static_cast<T>(std::llround(d));
            } else {
                value = static_cast<T>(d);
            }
        });
    }
    Control add(Kind kind, const std::string& id, const std::string& label, Getter get, Setter set);
    Control value_getter(const std::string& id, const std::string& label, Getter get);

    Dashboard& owner_;
    std::string title_;
    std::vector<std::string> ids_;
    std::vector<std::string> command_ids_;
};

class Dashboard {
public:
    explicit Dashboard(std::string title = {});
    ~Dashboard();

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    void title(std::string title) { title_ = std::move(title); }
    const std::string& title() const { return title_; }

    /// Group (card) of controls; created on first use.
    Group& group(const std::string& title);

    /// Link shown in the dashboard header (e.g. to your own pages).
    void link(const std::string& label, const std::string& href);

    bool contains(const std::string& id) const;
    std::vector<std::string> ids() const;
    const detail::ControlDef& control(const std::string& id) const;

    // --- runtime; call on the application main loop ------------------------
    /// Full description of the dashboard including current values.
    json schema() const;
    /// Current value of every stateful control: {"id": value, ...}.
    json state() const;
    json get(const std::string& id) const;
    /// Validate and apply a value; for buttons this presses them.
    /// Throws forge::Error (404 unknown id, 400 invalid value, 403 read-only).
    void set(const std::string& id, const json& value);
    void press(const std::string& id);

    // --- commands --------------------------------------------------------------
    bool has_command(const std::string& id) const;
    std::vector<std::string> command_ids() const;
    const detail::CommandDef& command(const std::string& id) const;
    /// Validate `args` (object), apply defaults and run the handler.
    /// Throws forge::Error (404 unknown, 400 invalid arguments).
    json call(const std::string& id, const json& args);
    /// Ids of controls marked persist().
    std::vector<std::string> persistent_ids() const;
    /// Values that changed since the previous call (for live updates).
    json changes();

    /// From now on, values (schema, state, get, set, press, call, changes) may
    /// only be used on `thread`: the controls read and write the application's
    /// state, which belongs to its main loop. forge::Application binds it to
    /// the main loop after setup_web(); other threads use app.invoke() / post().
    /// A call from another thread throws std::logic_error naming the method.
    void bind_to_thread(std::thread::id thread) { owner_ = thread; }

private:
    friend class Group;
    detail::ControlDef& add(detail::ControlDef def);

    std::string title_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<detail::ControlDef>> controls_;
    std::map<std::string, detail::ControlDef*> by_id_;
    std::vector<std::unique_ptr<detail::CommandDef>> commands_;
    std::map<std::string, detail::CommandDef*> commands_by_id_;
    std::vector<std::pair<std::string, std::string>> links_;
    json last_state_ = json::object();
    std::thread::id owner_{};
    void check_thread(const char* method) const;
};

}  // namespace forge::web
