# 6. Streams and Python clients

A **stream** publishes messages on `[forge].data` (ZeroMQ PUB), from any
thread. Each message is `[topic][forge.v1.Header][body][blob]`: the header
carries the source, a sequence number (gaps = lost messages) and the trace
context; the blob carries raw samples without copying.

```cpp
readings_ = &stream("pressure", "forge.v1.Values");    // in on_start()
...
if (readings_->wanted()) {                              // someone listens
    forge::v1::Values v;
    (*v.mutable_values())["pressure_bar"] = forge::proto::to_value(pressure_bar);
    readings_->publish(v);
}
```

Your own messages: a `.proto` file and `forge_add_proto(pump PROTOS
pump/v1/pump.proto PYTHON ${CMAKE_CURRENT_BINARY_DIR}/python)`
(`forgecpp-new` sets this up). `zmq::StreamOptions` limits the rate or keeps
only the latest message for slow subscribers. `forgectl streams` lists them.

**Python** (`forge.v1` comes with forgecpp, in site-packages; your messages
with `forge_install_python(DIRECTORY ...)`):

```python
import zmq
from forge.v1 import framing, common_pb2

s = zmq.Context().socket(zmq.SUB)
s.setsockopt(zmq.SUBSCRIBE, b"pressure")
s.connect("tcp://pump-host:5578")
while True:
    topic, header, values, _ = framing.parse(s.recv_multipart(), common_pb2.Values)
    print(header.seq, values.values["pressure_bar"].number_value)
```

C++ clients use `forge::zmq::Receiver`; control requests from any language
go to `[forge].control` as `forge.v1.Request` (or HTTP: `/forge/api/rpc`).
Next: [deploying](07-deploy.md).
