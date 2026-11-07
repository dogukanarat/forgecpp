#pragma once
// Unit testing helpers on top of doctest.
//
// Link against forgecpp::testing (or use forge_add_test() in CMake), which
// provides main(); then just write TEST_CASEs:
//
//   #include <forge/testing.hpp>
//   TEST_CASE("config defaults") {
//       auto cfg = forge::Config::parse("port = 80");
//       CHECK(cfg.get_or<int>("port", 0) == 80);
//   }

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace forge::testing {

/// Temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            auto candidate = base / ("forgecpp-test-" + std::to_string(rd()));
            if (std::filesystem::create_directory(candidate)) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create temporary directory");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

/// $FORGE_TEST_TIMEOUT_SCALE (default 1): slow builds (sanitizers, emulation)
/// multiply the timeouts of wait_until() with it instead of each test guessing.
inline double timeout_scale() {
    static const double scale = [] {
        const char* s = std::getenv("FORGE_TEST_TIMEOUT_SCALE");
        const double v = s ? std::atof(s) : 1.0;
        return v >= 1.0 ? v : 1.0;
    }();
    return scale;
}

/// Poll `predicate` until it is true or `timeout` (times timeout_scale()) expires.
inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::milliseconds>(timeout * timeout_scale());
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

/// Runs an application on a background thread for a test, isolated from the
/// working directory (empty config file) and with the console log off.
/// Endpoints in `args` should use ephemeral ports ("tcp://127.0.0.1:*", port 0).
///
///   forge::testing::AppRunner<MyDaemon> r({"-D", "forge.control=tcp://127.0.0.1:*"});
///   forge::zmq::Client c(r.app().endpoints().at("control"));
///   ...                                   // stopped and joined on destruction
template <class App>
class AppRunner {
public:
    explicit AppRunner(std::vector<std::string> args = {}, std::unique_ptr<App> app = std::make_unique<App>())
        : app_(std::move(app)) {
        const auto config = dir_ / "empty.toml";
        std::ofstream(config) << "# test configuration: everything comes from -D\n";
        // No registration with a hub that may run on the machine (pass -D hub.register=true to test it).
        // Card layouts stay in the test's directory, not in ~/.local/state.
        args_ = {"app", "-c", config.string(), "-D", "log.console=false", "-D", "hub.register=false",
                 "-D", "web.layout_file=" + (dir_ / "web-layout.json").string()};
        args_.insert(args_.end(), args.begin(), args.end());
        for (auto& a : args_) argv_.push_back(a.data());
        auto started = std::make_shared<std::promise<void>>();
        auto future = started->get_future();
        app_->hook_start([started](auto& a) { a.post([started] { started->set_value(); }); });
        thread_ = std::thread([this] { exit_code_ = app_->run(static_cast<int>(argv_.size()), argv_.data()); });
        if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
            throw std::runtime_error("application did not start within 20 s");
        }
    }
    ~AppRunner() { stop(); }
    AppRunner(const AppRunner&) = delete;
    AppRunner& operator=(const AppRunner&) = delete;

    App& app() { return *app_; }
    App* operator->() { return app_.get(); }
    /// Stop and join; returns the exit code of run().
    int stop() {
        if (thread_.joinable()) {
            app_->stop();
            thread_.join();
        }
        return exit_code_;
    }
    const std::filesystem::path& dir() const { return dir_.path(); }

private:
    TempDir dir_;
    std::unique_ptr<App> app_;
    std::vector<std::string> args_;
    std::vector<char*> argv_;
    std::thread thread_;
    int exit_code_ = -1;
};

}  // namespace forge::testing
