# 6. Streams, commands and Python clients

A **stream** publishes messages on `[forge].data` (ZeroMQ PUB), from any
thread. Each message is `[topic][forge.stream_header][body][blob]`: the
header carries the source, a sequence number (gaps = lost messages) and the
trace context; the blob carries raw samples without copying.

The pump's messages are in `pump.proto`. Every string, bytes and repeated
field needs a bound (`[(nanopb).max_count = 32]`): a message is a plain C
struct and nothing allocates.

```proto
syntax = "proto3";
import "nanopb.proto";
package pump;

message reading {
    double pressure_bar = 1;
    double speed_rpm = 2;
    bool running = 3;
}
```

```cmake
forge_add_proto(pump ROOT pump PROTOS pump.proto PYTHON ${CMAKE_CURRENT_BINARY_DIR}/python)   # pump_pb2.py too
```

```cpp
#include "pump.forge.hpp"

readings_ = &stream<pump_reading>("pressure");          // in on_start()
...
if (readings_->wanted()) {                              // someone listens
    readings_->publish(pump_reading{pressure_bar, speed_rpm, running});
}
```

`zmq::StreamOptions` limits the rate or keeps only the latest message for
slow subscribers. `forgectl streams` lists them. `forgecpp-new` sets all of
this up.

**Commands of your own** go in the same file: a request, a reply and a
number from 0x0100 (0x0001..0x00FF are forgecpp's). One handler answers the
binary link and the web's JSON. The pump's `history` returns the last
readings:

```proto
enum msg_id { none = 0; history = 0x0100; }
message history_request { uint32 count = 1; }
message history_reply { repeated double pressure_bar = 1 [(nanopb).max_count = 32]; }
```

```cpp
// In the constructor: the handler runs on the main loop.
command<pump_history_request, pump_history_reply>(pump_msg_id_history, "history", [this](const auto& req, auto& rsp) {
    const auto n = std::min<std::size_t>(req.count, history.size());
    return forge::pb::assign(rsp.pressure_bar, rsp.pressure_bar_count, std::span(history).last(n));  // E2BIG above 32
}, forge::Role::viewer);  // a query; a command that changes something needs operator and is audited
```

```sh
curl -X POST http://pump-host:8080/forge/api/commands/history -d '{"count": 3}'   # {"pressure_bar": [4.1, 4.3, 4.4]}
```

**Python** (the `forge` package comes with forgecpp, in site-packages; your
messages with `forge_install_python(DIRECTORY ...)`):

```python
import zmq
from forge import c2, framing
import pump_pb2                                   # <build>/python on PYTHONPATH

s = zmq.Context().socket(zmq.SUB)
s.setsockopt(zmq.SUBSCRIBE, b"pressure")
s.connect("tcp://pump-host:5578")
topic, header, r, _ = framing.parse(s.recv_multipart(), pump_pb2.reading)
print(header.seq, r.pressure_bar)

link = c2.Client("tcp://pump-host:5577")
print(link.call(pump_pb2.history, pump_pb2.history_request(count=3), pump_pb2.history_reply).pressure_bar)
```

C++ clients use `forge::zmq::Receiver` for streams, `forge::Remote` for
controls and `forge::zmq::Client` for any command.
Next: [deploying](07-deploy.md).
