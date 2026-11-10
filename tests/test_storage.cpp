// forgecpp::sqlite (Database, Backlog) and forgecpp::lz4.
#include <forge/backlog.hpp>
#include <forge/lz4.hpp>
#include <forge/sqlite.hpp>
#include <forge/testing.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace sqlite = forge::sqlite;
namespace lz4 = forge::lz4;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes bytes(std::string_view s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("sqlite: statements, columns, transactions") {
    forge::testing::TempDir dir;
    sqlite::Database db((dir / "t.db").string());
    db.exec("CREATE TABLE t(k TEXT PRIMARY KEY, n INTEGER, x REAL, b BLOB)");
    auto put = db.prepare("INSERT INTO t VALUES(?, ?, ?, ?)");
    put.bind(1, "a").bind(2, 42).bind(3, 2.5).bind(4, std::span<const std::uint8_t>(Bytes{1, 0, 2})).run();
    put.bind(1, "b").bind(2, nullptr).bind(3, 0.0).bind(4, std::span<const std::uint8_t>()).run();
    CHECK(db.changes() == 1);

    auto get = db.prepare("SELECT n, x, b FROM t WHERE k = ?");
    REQUIRE(get.bind(1, "a").step());
    CHECK(get.integer(0) == 42);
    CHECK(get.real(1) == 2.5);
    CHECK(get.blob(2) == Bytes{1, 0, 2});
    CHECK_FALSE(get.step());
    get.reset();
    REQUIRE(get.bind(1, "b").step());
    CHECK(get.null(0));
    CHECK(get.blob(2).empty());
    CHECK_FALSE(get.null(2));  // an empty blob is not NULL

    {
        sqlite::Transaction tx(db);
        db.exec("DELETE FROM t");
    }  // no commit: rolled back
    auto count = db.prepare("SELECT COUNT(*) FROM t");
    REQUIRE(count.step());
    CHECK(count.integer(0) == 2);

    CHECK_THROWS_WITH_AS(db.prepare("SELECT nope FROM t"), doctest::Contains("no such column"), sqlite::Error);
    CHECK_THROWS_AS(put.bind(1, "a").run(), sqlite::Error);  // the key is taken
    CHECK_FALSE(sqlite::version().empty());
}

TEST_CASE("sqlite: a file that cannot be opened says which one") {
    CHECK_THROWS_WITH_AS(sqlite::Database("/nonexistent-dir/x.db"), doctest::Contains("/nonexistent-dir/x.db"),
                         sqlite::Error);
}

TEST_CASE("backlog: records wait in order and stay after a restart") {
    forge::testing::TempDir dir;
    const auto path = (dir / "backlog.db").string();
    std::int64_t first = 0;
    {
        forge::Backlog b(path);
        first = b.append("hk", "one");
        b.append("hk", "two");
        b.append("event", bytes("three"));
        CHECK(b.size() == 3);
        CHECK(b.bytes() == 11);
    }
    forge::Backlog b(path);  // the program started again
    CHECK(b.size() == 3);
    CHECK(b.bytes() == 11);
    auto batch = b.peek(2);
    REQUIRE(batch.size() == 2);
    CHECK(batch[0].id == first);
    CHECK(batch[0].topic == "hk");
    CHECK(batch[0].data == bytes("one"));
    CHECK(batch[1].data == bytes("two"));
    CHECK(batch[0].time_ms > 0);

    b.remove_through(batch.back().id);  // the other end has them
    CHECK(b.size() == 1);
    CHECK(b.bytes() == 5);
    batch = b.peek(10);
    REQUIRE(batch.size() == 1);
    CHECK(batch[0].topic == "event");
    // An id is never used twice, also after the newest went.
    b.remove_through(batch[0].id);
    CHECK(b.append("hk", "four") > batch[0].id);
}

TEST_CASE("backlog: peek stops at about max_bytes, but returns at least one record") {
    forge::testing::TempDir dir;
    forge::Backlog b((dir / "b.db").string());
    for (int i = 0; i < 10; ++i) b.append("x", std::string(100, 'a'));
    CHECK(b.peek(100, 350).size() == 3);
    CHECK(b.peek(100, 10).size() == 1);
    CHECK(b.peek(0).empty());
}

TEST_CASE("backlog: when full, the oldest records go first") {
    forge::testing::TempDir dir;
    forge::Backlog b((dir / "b.db").string(), {.max_records = 5, .max_bytes = 1000});
    std::int64_t last = 0;
    for (int i = 0; i < 8; ++i) last = b.append("x", std::to_string(i));
    CHECK(b.size() == 5);
    CHECK(b.dropped() == 3);
    const auto batch = b.peek(10);
    REQUIRE(batch.size() == 5);
    CHECK(batch.front().data == bytes("3"));
    CHECK(batch.back().id == last);

    // The byte limit too.
    b.append("big", std::string(990, 'z'));
    CHECK(b.bytes() <= 1000);
    CHECK(b.peek(10).back().data.size() == 990);  // the newest always stays
}

TEST_CASE("backlog: several threads append at once") {
    forge::testing::TempDir dir;
    forge::Backlog b((dir / "b.db").string());
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&b, t] {
            for (int i = 0; i < 50; ++i) b.append("t" + std::to_string(t), std::to_string(i));
        });
    }
    for (auto& t : threads) t.join();
    CHECK(b.size() == 200);
    const auto all = b.peek(1000);
    for (std::size_t i = 1; i < all.size(); ++i) CHECK(all[i].id > all[i - 1].id);
}

TEST_CASE("lz4: data comes back as it was, smaller when it repeats") {
    std::string text;
    for (int i = 0; i < 1000; ++i)
        text += R"({"t":)" + std::to_string(i) +
                R"(,"v":21.5,"state":"nominal"})"
                "\n";
    const auto packed = lz4::compress(text);
    CHECK(packed.size() < text.size() / 4);
    Bytes out;
    REQUIRE(lz4::decompress(packed, out).ok());
    CHECK(std::string(out.begin(), out.end()) == text);

    // Random data does not shrink, and still comes back.
    std::mt19937 rng(1);
    Bytes noise(100000);
    for (auto& b : noise) b = static_cast<std::uint8_t>(rng());
    out.clear();
    REQUIRE(lz4::decompress(lz4::compress(noise, {.level = 9}), out).ok());
    CHECK(out == noise);

    out.clear();
    REQUIRE(lz4::decompress(lz4::compress(Bytes{}), out).ok());
    CHECK(out.empty());
}

TEST_CASE("lz4: damaged, cut or too large data is refused and leaves nothing") {
    const auto packed = lz4::compress(std::string(50000, 'x') + "end");
    Bytes out{9};
    CHECK(lz4::decompress(Bytes{1, 2, 3, 4, 5}, out).errno_code() == EBADMSG);
    CHECK(lz4::decompress(std::span(packed).first(packed.size() - 6), out).errno_code() == EBADMSG);
    auto damaged = packed;
    damaged[damaged.size() / 2] ^= 0xFF;
    CHECK_FALSE(lz4::decompress(damaged, out).ok());
    CHECK(lz4::decompress(packed, out, 1000).errno_code() == EMSGSIZE);  // a limit for untrusted input
    CHECK(out == Bytes{9});  // what was there before stays, nothing is added
}

#ifdef FORGE_LZ4_TOOL
TEST_CASE("lz4: the lz4 command line tool reads what forge::lz4 writes, and the other way") {
    forge::testing::TempDir dir;
    const std::string text = "telemetry batch 1\n" + std::string(4000, '.');
    const auto packed = lz4::compress(text);
    {
        std::ofstream f(dir / "a.lz4", std::ios::binary);
        f.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
    }
    const auto a = (dir / "a.lz4").string(), a_out = (dir / "a.txt").string();
    REQUIRE(std::system((std::string(FORGE_LZ4_TOOL) + " -q -d -f " + a + " " + a_out).c_str()) == 0);
    std::ifstream back(a_out, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(back), {}) == text);

    const auto b_in = (dir / "b.txt").string(), b = (dir / "b.lz4").string();
    std::ofstream(b_in) << text;
    REQUIRE(std::system((std::string(FORGE_LZ4_TOOL) + " -q -f " + b_in + " " + b).c_str()) == 0);
    std::ifstream f(b, std::ios::binary);
    const Bytes from_tool((std::istreambuf_iterator<char>(f)), {});
    Bytes out;
    REQUIRE(lz4::decompress(from_tool, out).ok());
    CHECK(std::string(out.begin(), out.end()) == text);
}
#endif
