#include <forge/json.hpp>
#include <forge/testing.hpp>

TEST_CASE("json: atomic save and load") {
    forge::testing::TempDir dir;
    const auto path = dir / "state/nested/state.json";
    forge::save_json(path, forge::json{{"count", 3}, {"names", {"a", "b"}}});
    const auto loaded = forge::load_json(path);
    CHECK(loaded["count"] == 3);
    CHECK(loaded["names"].size() == 2);
    CHECK_THROWS(forge::load_json(dir / "missing.json"));
}
