// The telemetry-store example: records wait while there is no contact, go
// out compressed during a pass, and survive a restart.
#include <forge/forge.hpp>
#include <forge/testing.hpp>

#include <fstream>
#include <iterator>

// main.cpp ends with FORGE_MAIN(TelemetryStore); the test has its own main().
#undef FORGE_MAIN
#define FORGE_MAIN(AppClass)
#include "main.cpp"

namespace {

using Runner = forge::testing::AppRunner<TelemetryStore>;

std::vector<std::string> args(const forge::testing::TempDir& dir) {
    return {"-D", "http.enabled=false",
            "-D", "telemetry.backlog=" + (dir / "backlog.db").string(),
            "-D", "telemetry.outbox=" + (dir / "outbox").string(),
            "-D", "telemetry.downlink_bps=0",
            "-D", "telemetry.batch_bytes=4096"};
}

std::size_t waiting(Runner& r) { return r->backlog().size(); }

}  // namespace

TEST_CASE("telemetry-store: records wait without contact and go out compressed during a pass") {
    forge::testing::TempDir dir;
    Runner r(args(dir), {.manual_time = true});
    r.advance(10s);  // 100 records, no contact
    CHECK(waiting(r) == 100);
    CHECK(std::filesystem::is_empty(dir / "outbox"));

    r->invoke([&] { r->dashboard()->set("contact", true); });  // the pass begins
    REQUIRE(forge::testing::wait_until([&] { return waiting(r) == 0; }));
    r->invoke([&] { r->dashboard()->set("contact", false); });

    // Every record is in the outbox, once, in order, in LZ4 files.
    std::vector<std::string> lines;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir / "outbox")) files.push_back(e.path());
    std::sort(files.begin(), files.end());
    CHECK(files.size() > 1);  // 4 kB batches
    std::size_t packed = 0, raw = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        std::vector<std::uint8_t> text;
        REQUIRE(forge::lz4::decompress(bytes, text).ok());
        packed += bytes.size();
        raw += text.size();
        std::string all(text.begin(), text.end());
        for (std::size_t at = 0; at < all.size();) {
            const auto end = all.find('\n', at);
            lines.push_back(all.substr(at, end - at));
            at = end + 1;
        }
    }
    REQUIRE(lines.size() == 100);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        CHECK(forge::json::parse(lines[i])["seq"] == i + 1);
    }
    CHECK(raw > 2 * packed);  // housekeeping repeats itself
    CHECK(r->invoke([&] { return r->dashboard()->get("records_sent"); }) == 100);
}

TEST_CASE("telemetry-store: the backlog is still there after a restart") {
    forge::testing::TempDir dir;
    {
        Runner r(args(dir), {.manual_time = true});
        r.advance(5s);
        CHECK(waiting(r) == 50);
    }
    Runner r(args(dir), {.manual_time = true});
    CHECK(waiting(r) == 50);
    r.advance(1s);
    CHECK(waiting(r) == 60);
}

TEST_CASE("telemetry-store: when the backlog is full the oldest records go") {
    forge::testing::TempDir dir;
    auto a = args(dir);
    a.insert(a.end(), {"-D", "telemetry.max_records=20"});
    Runner r(a, {.manual_time = true});
    r.advance(5s);
    CHECK(waiting(r) == 20);
    CHECK(r->backlog().dropped() == 30);
    const auto oldest = r->backlog().peek(1);
    REQUIRE(oldest.size() == 1);
    const auto first = forge::json::parse(std::string(oldest[0].data.begin(), oldest[0].data.end()));
    CHECK(first["seq"] == 31);
}
