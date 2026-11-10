// An application for the forge-hub tests: registers like every forgecpp
// application and misbehaves on request.
//
//   forge-hub-test-app --name a --crash-after-ms 500     # SIGKILL itself
//   forge-hub-test-app --name b --hang-after-ms 500      # block the main loop
//   forge-hub-test-app --name c --exit-after-ms 500 --exit-code 3
#include <forge/forge.hpp>

#include <csignal>
#include <cstring>
#include <thread>

#include <sys/prctl.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {
std::string name_from(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--name") == 0) return argv[i + 1];
    }
    return "hub-test-app";
}

class TestApp : public forge::Application {
public:
    explicit TestApp(std::string name) : Application({std::move(name), "1.2.3", "forge-hub test application"}) {}

protected:
    void setup_cli(CLI::App& cli) override {
        cli.add_option("--name", name_, "Application name");
        cli.add_option("--crash-after-ms", crash_ms_);
        cli.add_option("--hang-after-ms", hang_ms_);
        cli.add_option("--exit-after-ms", exit_ms_);
        cli.add_option("--exit-code", exit_code_);
    }
    void on_start() override {
        if (crash_ms_ > 0) after(std::chrono::milliseconds(crash_ms_), [] { ::kill(::getpid(), SIGKILL); });
        if (hang_ms_ > 0)
            after(std::chrono::milliseconds(hang_ms_), [] {
                for (;;) std::this_thread::sleep_for(1h);
            });
        if (exit_ms_ > 0) after(std::chrono::milliseconds(exit_ms_), [this] { stop(exit_code_); });
    }

private:
    std::string name_;
    int crash_ms_ = 0, hang_ms_ = 0, exit_ms_ = 0, exit_code_ = 0;
};
}  // namespace

int main(int argc, char** argv) {
    // Ends with the test that started it, even when that test crashed (no orphans
    // keeping ctest waiting for their output).
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) return 1;
    TestApp app(name_from(argc, argv));
    return app.run(argc, argv);
}
