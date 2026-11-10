# Storage and compression

Two optional components, each a shared library of its own, so that the core
library needs neither:

| Component | Library | Header | Needs |
|---|---|---|---|
| `forgecpp::sqlite` | `libforgecpp-sqlite.so` | `forge/sqlite.hpp`, `forge/backlog.hpp` | SQLite 3 (`third_party/sqlite`; Yocto: `sqlite3` of oe-core) |
| `forgecpp::lz4` | `libforgecpp-lz4.so` | `forge/lz4.hpp` | liblz4 (`third_party/lz4`; Yocto: `lz4` of oe-core) |

SQLite and LZ4 are submodules, built and installed by `install.sh` as shared
libraries like every other library (`--without-sqlite`, `--without-lz4`
leave them out). The components are built when their library is found
(`-DFORGECPP_WITH_SQLITE=OFF`, `-DFORGECPP_WITH_LZ4=OFF` leave them out). An application links the
component:

```cmake
find_package(forgecpp 0.4 REQUIRED COMPONENTS sqlite lz4)
forge_add_app(recorder SOURCES main.cpp LIBS forgecpp::sqlite forgecpp::lz4)
```

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
them compressed with LZ4 into an outbox through a link of a set rate. Its
test checks that every record arrives once and in order, that the backlog
survives a restart, and that the oldest records go when it is full.
