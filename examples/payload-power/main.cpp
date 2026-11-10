// payload-power: switch a payload on and off with two GPIO lines.
//
//   EN     (output) ----> the payload's power switch
//   PGOOD  (input)  <---- power-good from the payload's regulator
//
// Power-up is a state machine (docs/threading.md, "Sequences"):
//
//   off --power_up--> powering --PGOOD rises--> ready
//                        |  no PGOOD within [payload].boot_ms
//                        v
//                     retrying (EN low 1 s) --> powering ... after [payload].attempts: failed (alarm)
//
//   ready --PGOOD falls--> the payload lost power: alarm, and power-up again
//
// A worker thread waits for the PGOOD edges and hands them to the main loop,
// where the state lives.
//
// [payload].chip = "sim" (the default) runs on any PC: the program plays the
// payload too (PGOOD follows EN 300 ms later while "Simulated payload
// answers" is on, and "Brown-out" drops it). On a board:
//
//   [payload]
//   chip = "gpiochip0"        # or the chip's label; gpiodetect lists them
//   en = "PAYLOAD_EN"         # line names from the device tree (gpioinfo), or offsets ("17")
//   pgood = "PAYLOAD_PGOOD"
//
//   ./payload-power --http 8080       # panel: http://localhost:8080/forge/

#include <forge/forge.hpp>

#include <atomic>
#include <memory>
#include <string>

using namespace std::chrono_literals;

class PayloadPower : public forge::Application {
public:
    enum class State { off, powering, retrying, ready, failed };

    PayloadPower() : Application({"payload-power", "1.0.0", "Payload power control over GPIO"}) {}

    State state() const { return state_; }  // main loop

protected:
    void setup_config(forge::config::Schema& s) override {
        s.section("payload", "The payload's power lines");
        s.string("payload.chip", "sim", "GPIO chip: \"gpiochip0\", its label, or \"sim\" (simulated)");
        s.string("payload.en", "PAYLOAD_EN", "Power switch line (name or offset)");
        s.string("payload.pgood", "PAYLOAD_PGOOD", "Power-good line (name or offset)");
        s.boolean("payload.en_active_low", false, "The switch is on when EN is low");
        s.integer("payload.boot_ms", 2000, "How long PGOOD may take after EN").range(10, 60000).unit("ms");
        s.integer("payload.attempts", 3, "Power-up attempts before giving up").range(1, 10);
        s.boolean("payload.power_on_start", true, "Power the payload up when the program starts");
    }

    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Payload");
        g.value("state", "State", [this] { return to_string(state_); });
        g.value("attempts", "Attempt", attempts_);
        g.value("en", "EN", [this] { return en_ && en_->get(); });
        g.value("pgood", "PGOOD", [this] { return pgood_ && pgood_->get(); });
        g.button("power_up", "Power up", [this] { power_up(); });
        g.button("power_off", "Power off", [this] { enter(State::off); });

        if (simulated()) {
            auto& sim = ui.group("Simulated payload");
            sim.toggle("sim_answers", "Payload answers", sim_answers_);
            sim.button("brown_out", "Brown-out", [this] { forge::gpio::sim::drive(chip_, pgood_line_, false); });
        }
    }

    void on_start() override {
        const auto cfg = config();
        chip_ = cfg.value<std::string>("payload.chip");
        pgood_line_ = cfg.value<std::string>("payload.pgood");
        boot_ = std::chrono::milliseconds(cfg.value<int>("payload.boot_ms"));
        max_attempts_ = cfg.value<int>("payload.attempts");

        // A line that is not there, or in use, stops the start with the reason.
        en_ = std::make_unique<forge::gpio::Output>(
            chip_, cfg.value<std::string>("payload.en"),
            forge::gpio::OutputOptions{.active_low = cfg.value<bool>("payload.en_active_low"),
                                       .consumer = "payload-power"});
        pgood_ = std::make_unique<forge::gpio::Input>(
            chip_, pgood_line_,
            forge::gpio::InputOptions{.edge = forge::gpio::Edge::both, .consumer = "payload-power"});

        // PGOOD's edges, from a thread of their own into the main loop.
        worker("pgood", [this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (auto e = pgood_->wait(100ms)) post([this, up = e->rising] { on_pgood(up); });
            }
        });

        if (cfg.value<bool>("payload.power_on_start")) power_up();
    }

    void on_stop() override {
        if (en_) en_->set(false);  // the payload goes off with the program
    }

private:
    bool simulated() const {
        const auto chip = config().value<std::string>("payload.chip");
        return chip == "sim" || chip.starts_with("sim:");
    }

    void power_up() {
        attempts_ = 0;
        enter(State::powering);
    }

    // Every transition goes through here.
    void enter(State next) {
        cancel(timer_);
        logger()->info("payload: {} -> {}", to_string(state_), to_string(next));
        state_ = next;
        switch (state_) {
            case State::off:
                switch_power(false);
                health().set("payload", forge::HealthState::ok, "off", false);
                break;
            case State::powering:
                ++attempts_;
                switch_power(true);
                timer_ = after(boot_, [this] {
                    logger()->warn("payload: no power-good after {} ms (attempt {} of {})", boot_.count(), attempts_,
                                   max_attempts_);
                    enter(attempts_ < max_attempts_ ? State::retrying : State::failed);
                });
                break;
            case State::retrying:  // off for a second, then the next attempt
                switch_power(false);
                timer_ = after(1s, [this] { enter(State::powering); });
                break;
            case State::ready:
                alarms().clear("payload.power");
                health().set("payload", forge::HealthState::ok, "powered", false);
                break;
            case State::failed:
                switch_power(false);
                alarms().raise("payload.power", forge::Severity::error,
                               "no power-good after " + std::to_string(max_attempts_) + " attempts");
                health().set("payload", forge::HealthState::failed, "does not power up", false);
                break;
        }
    }

    void on_pgood(bool up) {
        if (state_ == State::powering && up) {
            enter(State::ready);
        } else if (state_ == State::ready && !up) {
            alarms().raise("payload.power", forge::Severity::warning, "the payload lost power");
            power_up();  // try to bring it back
        }
    }

    void switch_power(bool on) {
        en_->set(on);
        if (!simulated()) return;
        // The simulated payload: PGOOD follows EN after its regulator's 300 ms.
        if (!on) {
            forge::gpio::sim::drive(chip_, pgood_line_, false);
        } else {
            after(300ms, [this] {
                if (en_->get() && sim_answers_.load()) forge::gpio::sim::drive(chip_, pgood_line_, true);
            });
        }
    }

    static const char* to_string(State s) {
        switch (s) {
            case State::off: return "off";
            case State::powering: return "powering";
            case State::retrying: return "retrying";
            case State::ready: return "ready";
            case State::failed: return "failed";
        }
        return "?";
    }

    std::unique_ptr<forge::gpio::Output> en_;
    std::unique_ptr<forge::gpio::Input> pgood_;
    std::string chip_, pgood_line_;
    std::chrono::milliseconds boot_{2000};
    int max_attempts_ = 3;

    State state_ = State::off;  // main loop only
    int attempts_ = 0;
    forge::Scheduler::Id timer_ = 0;
    std::atomic<bool> sim_answers_{true};  // the panel writes it, a timer reads it
};

FORGE_MAIN(PayloadPower)
