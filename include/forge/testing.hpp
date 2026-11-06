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
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <thread>

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

/// Poll `predicate` until it is true or `timeout` expires.
inline bool wait_until(const std::function<bool()>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

}  // namespace forge::testing
