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

#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
    Getter get;
    Setter set;
    std::function<void()> action;
};
}  // namespace detail

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

    /// On/off switch.
    Control toggle(const std::string& id, const std::string& label, bool& value);
    Control toggle(const std::string& id, const std::string& label, std::function<bool()> get,
                   std::function<void(bool)> set);

    /// Slider between min and max.
    template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>>
    Control slider(const std::string& id, const std::string& label, T& value, double min, double max,
                   double step = 1) {
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
        return add(kind, id, label, [&value] { return json(value); },
                   [&value](const json& v) {
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
    /// Throws http::HttpError (404 unknown id, 400 invalid value, 403 read-only).
    void set(const std::string& id, const json& value);
    void press(const std::string& id);
    /// Values that changed since the previous call (for live updates).
    json changes();

private:
    friend class Group;
    detail::ControlDef& add(detail::ControlDef def);

    std::string title_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::vector<std::unique_ptr<detail::ControlDef>> controls_;
    std::map<std::string, detail::ControlDef*> by_id_;
    std::vector<std::pair<std::string, std::string>> links_;
    json last_state_ = json::object();
};

}  // namespace forge::web
