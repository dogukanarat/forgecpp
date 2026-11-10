# Storage and compression

Three optional components, so that the core library needs none of them:

| Component | Library | Header | Needs |
|---|---|---|---|
| `forgecpp::sqlite` | `libforgecpp-sqlite.so` | `forge/sqlite.hpp`, `forge/backlog.hpp` | SQLite 3 (`third_party/sqlite`; Yocto: `sqlite3` of oe-core) |
| `forgecpp::lz4` | `libforgecpp-lz4.so` | `forge/lz4.hpp` | liblz4 (`third_party/lz4`; Yocto: `lz4` of oe-core) |
| `forgecpp::orm` | none (headers) | `forge/orm.hpp` | sqlite_orm 1.9.1 (`third_party/sqlite_orm`; Yocto: `sqlite-orm` of this layer) and SQLite 3. **AGPL-3.0**, see [below](#license-of-sqlite_orm) |

SQLite, LZ4 and sqlite_orm are submodules, built and installed by
`install.sh` like every other library: SQLite and LZ4 as shared libraries,
sqlite_orm as its header (`--without-sqlite`, `--without-lz4`,
`--without-sqlite-orm` leave them out). The components are there when their
library is found (`-DFORGECPP_WITH_SQLITE=OFF`, `-DFORGECPP_WITH_LZ4=OFF`,
`-DFORGECPP_WITH_SQLITE_ORM=OFF` leave them out). An application links the
component:

```cmake
find_package(forgecpp 0.4 REQUIRED COMPONENTS sqlite lz4)
forge_add_app(recorder SOURCES main.cpp LIBS forgecpp::sqlite forgecpp::lz4)
```

Which SQLite to use: `forge::sqlite` when the SQL is short or the queries
are hot (you write the SQL, it costs nothing more than SQLite);
`forge::orm` when the data is a few structs with fields and you would rather
not write SQL (the compiler checks the queries; it costs compile time, a few
seconds for a file that includes it). Both read and write the same files,
and one program may use both.

## SQLite (`forge/sqlite.hpp`)

A thin layer: a database, its statements, transactions, errors as
`forge::sqlite::Error` (with SQLite's message and code).

```cpp
forge::sqlite::Database db("/var/lib/recorder/data.db");
db.exec("CREATE TABLE IF NOT EXISTS passes(id INTEGER PRIMARY KEY, start_ms INTEGER, station TEXT)");
auto add = db.prepare("INSERT INTO passes(start_ms, station) VALUES(?, ?)");
add.bind(1, now_ms).bind(2, "ankara").run();

auto q = db.prepare("SELECT station, COUNT(*) FROM passes GROUP BY station");
while (q.step()) logger()->info("{}: {} passes", q.text(0), q.integer(1));
```

- **WAL and `synchronous=NORMAL` by default**: the file is never damaged; a
  crash of the program loses nothing that was committed, a power cut may
  lose the last commits. `{.full_sync = true}` waits for the disk on every
  commit (slower on flash).
- **One connection, one thread at a time.** Put a mutex around it, or open
  one per thread. A query that takes long belongs off the main loop
  (`background()`, a `worker()`).
- **Transactions** make many writes one write on flash:
  `forge::sqlite::Transaction tx(db); ...; tx.commit();` (rolled back if
  `commit()` is not reached).
- Files go under `StateDirectory` (`$STATE_DIRECTORY`, `/var/lib/<app>`), the
  place meant for data: the generated unit makes `/usr`, `/boot` and `/etc`
  read-only (`ProtectSystem=full`).

## Tables as structs (`forge/orm.hpp`, sqlite_orm)

[sqlite_orm](https://github.com/fnc12/sqlite_orm) maps a struct to a table
and writes the SQL: a query is a C++ expression, and a column name or type
that does not match is a compile error, not a run-time one. `forge::orm::open()`
gives the storage the settings `forge::sqlite::Database` has, and creates or
updates the tables.

```cpp
#include <forge/orm.hpp>

struct Fault {
    std::int64_t id = 0;
    std::int64_t time_ms = 0;
    int severity = 0;
    std::string text;
    std::optional<std::int64_t> cleared_ms;   // std::optional: a column that may be NULL
};

// The schema, once. decltype gives the storage's type for a member.
inline auto make_db(const std::string& path) {
    using namespace sqlite_orm;
    return make_storage(path,
        make_table("faults",
            make_column("id", &Fault::id, primary_key().autoincrement()),
            make_column("time_ms", &Fault::time_ms),
            make_column("severity", &Fault::severity),
            make_column("text", &Fault::text),
            make_column("cleared_ms", &Fault::cleared_ms)));
}
using Db = decltype(make_db(""));

Db db = make_db("/var/lib/app/faults.db");
forge::orm::open(db);                    // WAL, busy timeout, tables made or updated

using namespace sqlite_orm;
auto id = db.insert(Fault{.time_ms = now_ms, .severity = 2, .text = "battery low"});
auto open = db.get_all<Fault>(where(is_null(&Fault::cleared_ms)),
                              order_by(&Fault::id).desc(), limit(10));
db.update_all(set(c(&Fault::cleared_ms) = now_ms), where(c(&Fault::id) == id));
auto n = db.count<Fault>(where(c(&Fault::severity) >= 2));
db.remove_all<Fault>(where(c(&Fault::time_ms) < now_ms - week));
db.transaction([&] { /* several writes */ return true; });   // false: roll back
```

Joins and groups give tuples:
`db.select(columns(&Unit::name, count(&Fault::id)), left_join<Fault>(on(c(&Fault::unit_id) == &Unit::id)), group_by(&Unit::id))`.
BLOBs are `std::vector<char>`. Errors are `std::system_error` exceptions.

- **One thread.** A storage is not thread safe: use it from the main loop
  or from one worker, like a `forge::sqlite::Database`. `open()` keeps one
  connection open; without it sqlite_orm opens and closes the file around
  every query.
- **Changing the structs.** `open()` runs sqlite_orm's `sync_schema` with
  `preserve = true`: a new table is made, a new column is added, a removed
  column goes, and the rows stay. **Except** a new column that is `NOT NULL`
  without a default (a plain member, no `default_value()`): sqlite_orm can
  only drop the table and make it again, empty. `open()` checks first and
  throws `std::runtime_error` instead; give the column a `default_value(...)`
  or make it `std::optional`, or pass `{.allow_data_loss = true}` if the rows
  do not matter. The first program on a device and the update after it must
  agree on this: test the update with a file the old program wrote.
- **What it costs.** It is one header of 25 000 lines: 1.2 s to read, and
  each kind of query is a template to compile (examples/fault-log/main.cpp
  takes 15 s at -O3, telemetry-store's 11 s). Include it in the `.cpp` files
  that use it, not in a header many files include. At run time, a query costs what the same SQL
  costs (prepared, bound, stepped), plus building the statement text each
  time.

### License of sqlite_orm

sqlite_orm 1.7 and later is **AGPL-3.0**, or **MIT** when bought from its
author (see its `COMM-LICENSE`; it was BSD-3-Clause up to 1.6, from 2020).
The header is compiled into the program that includes it, so that program
is under AGPL-3.0 unless the MIT license is bought: its source must be
offered to whoever gets the program (and, AGPL, to whoever uses it over a
network). forgecpp itself, and every program that does not include
`forge/orm.hpp`, are not affected: no forgecpp library contains sqlite_orm.

For lab use this does not matter. Before a product ships, either buy the MIT
license, or keep sqlite_orm out of the product's programs (`forge::sqlite`
does the same work with SQL). Yocto lists the license in the image's
manifest (`sqlite-orm`: AGPL-3.0-only); [yocto/README.md](../yocto/README.md)
says how to leave it out.

## Store and forward (`forge/backlog.hpp`)

`forge::Backlog` keeps records in an SQLite file until the other end has
them: telemetry between two ground passes, measurements while the network
is down. They survive restarts and power cuts.

```cpp
forge::Backlog backlog("/var/lib/app/backlog.db", {.max_records = 100000});
backlog.append("hk", record);                       // any thread

// During contact:
auto batch = backlog.peek(1000, 16 * 1024);         // the oldest, about 16 kB
if (send(batch)) backlog.remove_through(batch.back().id);   // only once they arrived
```

- **Delivery is at least once**: records leave only with `remove_through()`,
  after the other end has them. A batch lost on the way is sent again; the
  ids (increasing, never reused) let the receiver drop duplicates.
- **When it is full** (`max_records`, `max_bytes`), the oldest records go
  first and `dropped()` counts them: the newest data is the most useful.
- Thread safe; each call is one transaction.

## LZ4 (`forge/lz4.hpp`)

Fast compression (hundreds of MB/s on a Cortex-A, less CPU than the radio
time it saves) in the **LZ4 frame format**, which the `lz4` tool and
Python's `lz4.frame` read and write:

```cpp
const auto packed = forge::lz4::compress(lines);          // {.level = 9}: smaller, slower
std::vector<std::uint8_t> back;
if (auto s = forge::lz4::decompress(packed, back, 1 << 20); !s.ok()) ...   // EBADMSG, EMSGSIZE
```

```sh
lz4 -dc batch-000001.jsonl.lz4 | head        # on the ground
```

- A content checksum is in every frame: damaged data is refused (`EBADMSG`),
  not returned.
- **Decompression has a limit** (`max_size`, 64 MiB by default): data from a
  link must not grow into all the memory there is (`EMSGSIZE`).
- Small inputs compress badly: a single 100-byte record grows. Compress
  batches (`examples/telemetry-store` waits for about 16 kB or a second).
- Text telemetry (JSON lines) typically shrinks 3 to 5 times; already
  compressed data (images, encrypted payloads) does not.

## The example

`examples/telemetry-store` puts both together: housekeeping records into a
backlog every 100 ms, and during a pass (the "Contact" control) batches of
them compressed with LZ4 into an outbox through a link of a set rate.

It also uses `forge::sqlite` directly, in the same file: a pass log, the
table `passes` (start, end, records and bytes sent), written with plain SQL
when a pass begins and ends (`prepare`, `bind`, `run`, a `Transaction` that
closes the pass and prunes the log together), counted at start with a
`SELECT`, and listed by the console's `passes` command:

```
telemetry-store> passes
pass          start (UTC)    length   records       sent
   2  2026-11-10 18:01:47       2 s        56        1 kB
   1  2026-11-10 18:01:39       5 s        66        2 kB
```

Its tests check that every record arrives once and in order, that the
backlog survives a restart, that the oldest records go when it is full, and
read the pass log back with a connection of their own
(`sqlite3 backlog.db "SELECT * FROM passes"` does the same).

`examples/fault-log` is the same kind of log without SQL, with `forge::orm`:
two tables (`units`, and `faults` with a foreign key to them), a dummy source
that raises faults now and then, info and warnings that clear themselves
after a while, errors that wait for an operator (a panel button, the
console's `clear`). The console's `faults`, `history [unit] [n]` and `stats`
are `get_all`, `select` with a join, and `group_by`:

```
fault-log> stats
unit        faults
adcs             4
eps              6
payload          2
radio            5
open warning  1
open error    3
```

Its tests raise and clear faults, run the console commands, restart the
program with faults still open, and check that the log keeps its newest
`[faults].keep` faults.
