// forgecpp_bench: how fast the paths a message takes are, on this machine.
//
//   build/tests/forgecpp_bench            # every case, about 10 s
//   build/tests/forgecpp_bench c2 serial  # the cases whose name starts with these
//
// Numbers are for comparing builds and machines (Release builds only), not
// limits: a link is slower than any of these in practice.
#include <forge/c2.hpp>
#include <forge/pb.hpp>
#include <forge/serial.hpp>
#include <forge/tmtc.hpp>
#include <forge/zmq.hpp>

#include "forge_test/commands.forge.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
namespace c2 = forge::c2;
namespace pb = forge::pb;

std::vector<std::string> wanted;

// Runs `once` for about `seconds` and prints the rate and the time of one.
void bench(const std::string& name, const std::function<void()>& once, double seconds = 1.0,
           std::size_t bytes_each = 0) {
    if (!wanted.empty()) {
        bool match = false;
        for (const auto& w : wanted) match = match || name.rfind(w, 0) == 0;
        if (!match) return;
    }
    for (int i = 0; i < 100; ++i) once();  // warm up
    std::size_t n = 0;
    const auto t0 = Clock::now();
    const auto until = t0 + std::chrono::duration<double>(seconds);
    while (Clock::now() < until) {
        for (int i = 0; i < 64; ++i) once();
        n += 64;
    }
    const double s = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("%-34s %12.0f /s %10.2f us", name.c_str(), n / s, s / n * 1e6);
    if (bytes_each) std::printf(" %10.1f MB/s", static_cast<double>(n * bytes_each) / s / 1e6);
    std::printf("\n");
}

Bytes request_frame(std::uint16_t id, const forge_test_echo_request& req) {
    std::array<std::uint8_t, 64> payload{};
    std::size_t n = 0;
    (void)pb::encode(req, payload, n);
    Bytes out(c2::header_size + n);
    std::size_t len = 0;
    (void)c2::encode_message(c2::make_header(c2::flag::req, id, 1, static_cast<std::uint16_t>(n)),
                             std::span(payload).first(n), out, len);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) wanted.emplace_back(argv[i]);
    std::printf("%-34s %15s %13s\n", "case", "rate", "each");

    // Messages: nanopb structs to bytes and back.
    forge_test_echo_request req{};
    req.token = 42;
    (void)pb::assign(req.text, "hello, forgecpp");
    std::array<std::uint8_t, 64> buf{};
    std::size_t len = 0;
    bench("pb encode (echo request)", [&] { (void)pb::encode(req, buf, len); });
    (void)pb::encode(req, buf, len);
    bench("pb decode (echo request)", [&] {
        forge_test_echo_request r{};
        (void)pb::decode(std::span(buf).first(len), r);
    });
    bench("pb to_json", [&] { (void)pb::to_json(req); });

    // A command: c2 frame in, handler, c2 frame out.
    forge::tmtc::Registry reg;
    reg.add<forge_test_echo_request, forge_test_echo_reply>(0x0101, "echo", [](const auto& rq, auto& rsp) {
        rsp.token = rq.token;
        return pb::assign(rsp.text, pb::view(rq.text));
    });
    const auto frame = request_frame(0x0101, req);
    forge::tmtc::Dispatcher d(reg);
    std::size_t answered = 0;
    bench("c2 dispatch (decode, run, encode)",
          [&] { d.handle(frame, [&](std::span<const std::uint8_t> m) { answered += m.size(); }); });

    // The serial framing (COBS, CRC, fragments) of a 1 kB message.
    Bytes message(1024);
    for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>(i * 13);
    forge::serial::Framing tx, rx;
    Bytes wire;
    bench("serial frame 1 kB (encode)", [&] {
        wire.clear();
        (void)tx.encode(message, wire);
    }, 1.0, message.size());
    wire.clear();
    (void)tx.encode(message, wire);
    std::size_t got = 0;
    bench("serial frame 1 kB (receive)",
          [&] { rx.receive(wire, [&](std::span<const std::uint8_t> m) { got += m.size(); }); }, 1.0, message.size());

    // A command over ZeroMQ (loopback TCP): request, answer.
    {
        forge::zmq::Server server("tcp://127.0.0.1:*", [&](std::string_view r) { return forge::zmq::answer(reg, r); });
        forge::zmq::Client client(server.endpoint());
        forge_test_echo_reply rsp{};
        bench("zmq command round trip (tcp)", [&] { (void)client.call(0x0101, req, rsp); });
    }

    // A stream: 64 kB blocks from a Publisher to a Receiver (loopback TCP).
    {
        forge::zmq::Publisher pub("tcp://127.0.0.1:*");
        auto& s = pub.add<forge_test_echo_request>("blocks");
        std::atomic<std::uint64_t> received{0};
        forge::zmq::Receiver::Options o;
        o.topics = {"blocks"};
        forge::zmq::Receiver sub(forge::zmq::Receiver::Kind::sub, pub.endpoint(),
                                 [&](forge::zmq::Message&& m) { received += m.blob.size(); }, o);
        while (s.subscribers() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto block = std::make_shared<std::string>(65536, 'x');
        if (wanted.empty() || std::string("zmq stream").rfind(wanted.front(), 0) == 0) {
            const auto t0 = Clock::now();
            std::size_t sent = 0;
            while (Clock::now() - t0 < std::chrono::seconds(1)) {
                s.publish(req, block, block->data(), block->size());
                ++sent;
            }
            // What arrived in that second and shortly after.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
            // A publisher drops what a slow subscriber cannot take: count what arrived.
            const double blocks = static_cast<double>(received) / static_cast<double>(block->size());
            std::printf("%-34s %12.0f /s %10.2f us %10.1f MB/s (%.0f%% of those sent arrived)\n",
                        "zmq stream 64 kB blocks (tcp)", blocks / secs, secs / blocks * 1e6,
                        static_cast<double>(received) / secs / 1e6, 100.0 * blocks / static_cast<double>(sent));
        }
    }
    std::printf("(%zu bytes answered, %zu received: the work was done)\n", answered, got);
    return 0;
}
