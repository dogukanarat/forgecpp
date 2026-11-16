// forgecpp::orm: forge::orm::open() and the sqlite_orm basics an application relies on.
#include <forge/orm.hpp>
#include <forge/sqlite.hpp>
#include <forge/testing.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace sqlite_orm;

namespace {

struct Item {
    std::int64_t id = 0;
    std::string name;
    double weight = 0;
    std::optional<std::int64_t> done_ms;
    std::vector<char> data;
};

auto make_db(const std::string& path) {
    return make_storage(path, make_table("items", make_column("id", &Item::id, primary_key().autoincrement()),
                                         make_column("name", &Item::name, unique()),
                                         make_column("weight", &Item::weight, default_value(0.0)),
                                         make_column("done_ms", &Item::done_ms), make_column("data", &Item::data)));
}
using Db = decltype(make_db(""));

// The same table as an older program had it: no weight, no done_ms.
struct OldItem {
    std::int64_t id = 0;
    std::string name;
    std::vector<char> data;
};
auto make_old_db(const std::string& path) {
    return make_storage(path,
                        make_table("items", make_column("id", &OldItem::id, primary_key().autoincrement()),
                                   make_column("name", &OldItem::name, unique()), make_column("data", &OldItem::data)));
}

// A newer program: one more column, NOT NULL and without a default.
struct NewItem {
    std::int64_t id = 0;
    std::string name;
    double weight = 0;
    std::optional<std::int64_t> done_ms;
    std::vector<char> data;
    std::string serial;
};
auto make_new_db(const std::string& path) {
    return make_storage(path, make_table("items", make_column("id", &NewItem::id, primary_key().autoincrement()),
                                         make_column("name", &NewItem::name, unique()),
                                         make_column("weight", &NewItem::weight, default_value(0.0)),
                                         make_column("done_ms", &NewItem::done_ms), make_column("data", &NewItem::data),
                                         make_column("serial", &NewItem::serial)));
}

}  // namespace

TEST_CASE("orm: open() keeps one connection with the forgecpp settings") {
    forge::testing::TempDir dir;
    const auto path = (dir / "items.db").string();
    Db db = make_db(path);
    const auto changes = forge::orm::open(db);
    CHECK(changes.at("items") == sync_schema_result::new_table_created);
    CHECK(db.is_opened());
    CHECK(db.pragma.journal_mode() == journal_mode::WAL);
    CHECK(db.pragma.synchronous() == 1);

    // Another connection sees the table and the mode (the file is shared, as in WAL it should be).
    forge::sqlite::Database raw(path, {.read_only = true});
    auto q = raw.prepare("PRAGMA journal_mode");
    REQUIRE(q.step());
    CHECK(q.text(0) == "wal");
}

TEST_CASE("orm: insert, query, update, remove without SQL") {
    forge::testing::TempDir dir;
    Db db = make_db((dir / "items.db").string());
    forge::orm::open(db);

    const auto a = db.insert(Item{.name = "a", .weight = 1.5, .data = {1, 0, 2}});
    const auto b = db.insert(Item{.name = "b", .weight = 3.0});
    db.insert(Item{.name = "c", .weight = 0.5, .done_ms = 7});
    CHECK(b == a + 1);
    CHECK(db.count<Item>() == 3);

    const auto item = db.get<Item>(a);
    CHECK(item.name == "a");
    CHECK(item.data == std::vector<char>{1, 0, 2});  // a BLOB, zeros and all
    CHECK_FALSE(item.done_ms.has_value());           // NULL

    auto open = db.get_all<Item>(where(is_null(&Item::done_ms)), order_by(&Item::weight).desc());
    REQUIRE(open.size() == 2);
    CHECK(open[0].name == "b");

    CHECK(db.total(&Item::weight) == doctest::Approx(5.0));

    // UPDATE ... SET done_ms = 9 WHERE done_ms IS NULL AND weight > 1
    db.update_all(set(c(&Item::done_ms) = 9), where(is_null(&Item::done_ms) and c(&Item::weight) > 1.0));
    CHECK(db.changes() == 2);
    CHECK(db.count<Item>(where(is_null(&Item::done_ms))) == 0);

    // A changed struct goes back whole.
    auto c_item = db.get_all<Item>(where(c(&Item::name) == "c")).at(0);
    c_item.weight = 10;
    db.update(c_item);
    CHECK(db.get<Item>(c_item.id).weight == 10);

    // Joined columns and groups: select() gives tuples.
    const auto rows =
        db.select(columns(&Item::done_ms, count(&Item::id)), group_by(&Item::done_ms), order_by(&Item::done_ms));
    REQUIRE(rows.size() == 2);
    CHECK(*std::get<0>(rows[0]) == 7);
    CHECK(std::get<1>(rows[1]) == 2);

    db.remove_all<Item>(where(c(&Item::id) <= b));
    CHECK(db.count<Item>() == 1);
    CHECK(db.get_pointer<Item>(a) == nullptr);

    // The name is unique: a second "c" fails, as a std::system_error.
    CHECK_THROWS_AS(db.insert(Item{.name = "c"}), std::system_error);
}

TEST_CASE("orm: a transaction is all or nothing") {
    forge::testing::TempDir dir;
    Db db = make_db((dir / "items.db").string());
    forge::orm::open(db);
    db.transaction([&] {
        db.insert(Item{.name = "kept"});
        return true;  // commit
    });
    db.transaction([&] {
        db.insert(Item{.name = "gone"});
        return false;  // roll back
    });
    CHECK_THROWS(db.transaction([&] {
        db.insert(Item{.name = "also gone"});
        db.insert(Item{.name = "kept"});  // unique: throws, the transaction rolls back
        return true;
    }));
    const auto names = db.select(&Item::name);
    CHECK(names == std::vector<std::string>{"kept"});
}

TEST_CASE("orm: new columns with a default or optional: the old rows stay") {
    forge::testing::TempDir dir;
    const auto path = (dir / "items.db").string();
    {
        auto old = make_old_db(path);
        forge::orm::open(old);
        old.insert(OldItem{.name = "from the old program", .data = {7}});
    }
    Db db = make_db(path);
    const auto changes = forge::orm::open(db);
    CHECK(changes.at("items") == sync_schema_result::new_columns_added);
    const auto all = db.get_all<Item>();
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "from the old program");
    CHECK(all[0].weight == 0.0);              // the new column's default
    CHECK_FALSE(all[0].done_ms.has_value());  // NULL
    CHECK(all[0].data == std::vector<char>{7});
}

TEST_CASE("orm: open() refuses a schema change that would lose the rows") {
    forge::testing::TempDir dir;
    const auto path = (dir / "items.db").string();
    {
        Db db = make_db(path);
        forge::orm::open(db);
        db.insert(Item{.name = "keep me"});
    }
    {
        auto db = make_new_db(path);  // "serial": NOT NULL, no default
        CHECK_THROWS_AS(forge::orm::open(db), std::runtime_error);
    }
    {
        Db db = make_db(path);
        forge::orm::open(db);
        CHECK(db.count<Item>() == 1);  // untouched
    }
    // Asked for: sqlite_orm drops the table and makes it again, empty.
    auto db = make_new_db(path);
    const auto changes = forge::orm::open(db, {.allow_data_loss = true});
    CHECK(changes.at("items") == sync_schema_result::dropped_and_recreated);
    CHECK(db.count<NewItem>() == 0);
}

TEST_CASE("orm: an in-memory database") {
    Db db = make_db(":memory:");
    forge::orm::open(db, {.wal = false});
    db.insert(Item{.name = "x"});
    CHECK(db.count<Item>() == 1);
}
