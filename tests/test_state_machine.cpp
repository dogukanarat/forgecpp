// The state machine of docs/threading.md ("Sequences"), with its test: the
// page shows this component, so it is compiled and run here.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <atomic>

using namespace std::chrono_literals;

namespace {

// Power up a payload: switch it on, give it time to boot, check that it
// answers, try three times, then give up with an alarm.
class Payload : public forge::Component {
public:
    enum class State { off, booting, checking, retrying, ready, failed };

    Payload() : Component("payload") {}

    void power_up() {  // a button, a command: on the main loop
        attempts_ = 0;
        enter(State::booting);
    }
    void abort() { enter(State::off); }
    State state() const { return state_; }

protected:
    void setup_controls(forge::Controls& ui) override {
        auto& g = ui.group("Payload");
        g.value("state", "State", [this] { return to_string(state_); });  // the panel and telemetry see it
        g.value("attempts", "Attempts", attempts_);
        g.button("power_up", "Power up", [this] { power_up(); });
        g.button("abort", "Abort", [this] { abort(); });
    }

private:
    // Every transition goes through here: one place to log, to cancel the
    // pending timer and to start what the new state needs.
    void enter(State next) {
        app().cancel(timer_);
        logger()->info("payload: {} -> {}", to_string(state_), to_string(next));
        state_ = next;
        switch (state_) {
            case State::off: switch_power(false); break;
            case State::booting:
                ++attempts_;
                switch_power(true);
                timer_ = app().after(2s, [this] { enter(State::checking); });
                break;
            case State::checking:
                // The probe blocks (a bus transaction): off the loop, the answer back on it.
                app().background([] { return probe(); }, [this, attempt = attempts_](bool alive) {
                    // An answer for an attempt that is over (aborted, started again): ignore it.
                    if (state_ != State::checking || attempts_ != attempt) return;
                    if (alive) {
                        enter(State::ready);
                    } else {
                        enter(attempts_ < 3 ? State::retrying : State::failed);
                    }
                });
                break;
            case State::retrying:  // off for a second, then the next attempt
                switch_power(false);
                timer_ = app().after(1s, [this] { enter(State::booting); });
                break;
            case State::ready: set_health(forge::HealthState::ok, "ready"); break;
            case State::failed:
                switch_power(false);
                set_health(forge::HealthState::failed, "no answer after 3 attempts");
                app().alarms().raise("payload.power", forge::Severity::error, "payload does not answer");
                break;
        }
    }

    static const char* to_string(State s) {
        switch (s) {
            case State::off: return "off";
            case State::booting: return "booting";
            case State::checking: return "checking";
            case State::retrying: return "retrying";
            case State::ready: return "ready";
            case State::failed: return "failed";
        }
        return "?";
    }

    void switch_power(bool on) { powered_ = on; }  // a GPIO in a real payload
    static bool probe() { return probe_answers.load(); }

    State state_ = State::off;
    int attempts_ = 0;
    bool powered_ = false;
    forge::Scheduler::Id timer_ = 0;

public:
    static inline std::atomic<bool> probe_answers{false};  // what probe() answers in the test
};

class PayloadApp : public forge::Application {
public:
    PayloadApp() : Application({"payload-test", "1.0.0", "State machine"}) { payload = &add_component<Payload>(); }
    Payload* payload = nullptr;
};

}  // namespace

TEST_CASE("docs: the payload state machine retries, gives up, and can be aborted") {
    using S = Payload::State;
    forge::testing::AppRunner<PayloadApp> r({"-D", "http.enabled=false"}, {.manual_time = true});
    const auto state = [&] { return r->invoke([&] { return r->payload->state(); }); };
    // The probe runs on the pool: wait for its answer to reach the loop.
    const auto probed = [&] { REQUIRE(forge::testing::wait_until([&] { return state() != S::checking; })); };

    Payload::probe_answers = false;
    r->invoke([&] { r->payload->power_up(); });
    CHECK(state() == S::booting);
    for (int attempt = 1; attempt <= 3; ++attempt) {
        r.advance(2s);  // boot time
        probed();
        if (attempt < 3) {
            CHECK(state() == S::retrying);
            r.advance(1s);
            CHECK(state() == S::booting);
        }
    }
    CHECK(state() == S::failed);
    CHECK(r->invoke([&] { return r->dashboard()->get("state"); }) == "failed");  // the panel sees it
    CHECK(r->invoke([&] { return r->alarms().highest_active(); }) == forge::Severity::error);

    Payload::probe_answers = true;
    r->invoke([&] { r->payload->power_up(); });
    r.advance(2s);
    probed();
    CHECK(state() == S::ready);

    r->invoke([&] {
        r->payload->power_up();
        r->payload->abort();
    });
    r.advance(5s);  // the boot timer was cancelled: nothing moves
    CHECK(state() == S::off);
}
