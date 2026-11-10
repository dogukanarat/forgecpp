#pragma once
// Components: reusable parts of an application with their own configuration
// keys, controls, console commands, health and lifecycle. An application is
// assembled from them, so the same building block (a radio backend, a stream
// tap, a sensor bus) looks and behaves the same in every application.
//
//   class SensorBus : public forge::Component {
//   public:
//       SensorBus() : Component("sensors") {}
//   protected:
//       void setup_config(forge::config::Schema& s) override {
//           s.string("sensors.port", "/dev/ttyUSB0", "Serial port");
//       }
//       void setup_controls(forge::Controls& ui) override {
//           ui.group("Sensors").value("temperature", "Temperature", temperature_);
//       }
//       void on_start() override {
//           app().every(1s, [this] { poll(); });
//       }
//       void poll() {
//           ...
//           set_health(ok ? forge::HealthState::ok : forge::HealthState::failed, error);
//       }
//   };
//
//   MyApp() : Application({...}) { add_component<SensorBus>(); }
//
// Hooks run in the order components were added, after the application's own
// setup_* (on_stop: before the application's, in reverse order).

#include "forge/health.hpp"
#include "forge/log.hpp"

#include <string>

namespace forge {

namespace detail {
struct ComponentAccess;
}  // namespace detail
class Application;
class Config;
class Console;
namespace config {
class Schema;
}  // namespace config
namespace web {
class Dashboard;
}  // namespace web
using Controls = web::Dashboard;
namespace http {
class Server;
}  // namespace http

class Component {
public:
    /// `critical`: the service is broken while this component has failed.
    explicit Component(std::string name, bool critical = true);
    virtual ~Component();
    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    const std::string& name() const { return name_; }
    bool critical() const { return critical_; }

protected:
    virtual void setup_config(config::Schema& schema);
    virtual void setup_controls(Controls& controls);
    /// The old name of setup_controls(); both are called.
    virtual void setup_web(web::Dashboard& dashboard);
    virtual void setup_console(Console& console);
    virtual void setup_http(http::Server& server);
    virtual void on_start();
    virtual void on_stop();
    virtual void on_reload(const Config& config);

    /// Valid from setup_config() on.
    Application& app();
    Logger logger() const;
    /// Report health under this component's name (thread-safe).
    void set_health(HealthState state, const std::string& message = {});

private:
    friend struct detail::ComponentAccess;  // the application that owns it calls the hooks
    Application* app_ = nullptr;
    std::string name_;
    bool critical_;
    Logger logger_;
};

}  // namespace forge
