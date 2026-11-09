// A small application for the browser tests of the panel (panel.test.js):
// one control of each kind, an alarm, a stream and a command.
#include <forge/forge.hpp>
#include <forge/messages.forge.hpp>

class PanelApp : public forge::Application {
public:
    PanelApp() : Application({"panel-test", "1.0.0", "Browser tests of the panel"}) {
        command<forge_ping_request, forge_ping_reply>(0x0100, "double_it", [](const auto& req, auto& rsp) {
            rsp.token = req.token * 2;
            return forge::Status();
        }, forge::Role::viewer);
    }

protected:
    void setup_web(forge::web::Dashboard& ui) override {
        auto& g = ui.group("Test");
        g.number("gain", "Gain", gain_).range(0, 70).unit("dB");
        g.toggle("enabled", "Enabled", enabled_);
        g.value("count", "Count", count_);
        g.button("bump", "Bump", [this] { logger()->warn("bumped to {}", ++count_); });
    }

    void on_start() override {
        alarms().raise("test.alarm", forge::Severity::warning, "raised by the test app");
        ticks_ = &stream<forge_log_record>("ticks");
        every(std::chrono::milliseconds(50), [this] {
            if (!ticks_->wanted()) return;
            forge_log_record r{};
            (void)forge::pb::assign(r.message, "tick " + std::to_string(++ticks_sent_));
            ticks_->publish(r);
        });
    }

private:
    double gain_ = 10;
    bool enabled_ = false;
    int count_ = 0;
    forge::zmq::Stream* ticks_ = nullptr;
    int ticks_sent_ = 0;
};

FORGE_MAIN(PanelApp)
