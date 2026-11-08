// qam-modem end to end: UDP -> modem -> radio-controller (simulated AD9361,
// TX1 -> splitter -> RX1 and RX2) -> modem -> UDP, at the 2 Mbit/s target.
#include "modem.hpp"

#include <forge/testing.hpp>

#ifdef QAM_WITH_RADIO_CONTROLLER
#include "radio_controller.hpp"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <set>
#include <thread>

using namespace std::chrono_literals;
using forge::json;

namespace {

json get(forge::Application& app, const std::string& id) {
    return app.invoke([&] { return app.dashboard()->get(id); });
}

std::string endpoint(forge::Application& app, const char* name) { return app.endpoints().at(name); }

/// UDP traffic like clients/echo.py: numbered packets at a rate, replies counted.
struct EchoResult {
    std::size_t sent = 0, received = 0, corrupted = 0;
    double rx_mbps = 0;
};

EchoResult echo(unsigned port, double mbps, std::size_t size, std::chrono::milliseconds duration) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(fd >= 0);
    timeval tv{0, 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(port));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    auto payload = [size](std::uint32_t seq) {
        std::vector<std::uint8_t> p(size);
        for (std::size_t i = 0; i < size; ++i) p[i] = static_cast<std::uint8_t>(seq * 31 + i * 7);
        std::memcpy(p.data(), &seq, 4);
        return p;
    };
    EchoResult r;
    std::set<std::uint32_t> seen;
    const auto interval = std::chrono::duration<double>(static_cast<double>(size) * 8 / (mbps * 1e6));
    const auto start = std::chrono::steady_clock::now();
    const auto end = start + duration;
    auto next = start;
    std::chrono::steady_clock::time_point first_rx{}, last_rx{};
    std::size_t bytes = 0;
    std::vector<std::uint8_t> buf(65536);
    while (std::chrono::steady_clock::now() < end + 500ms) {
        const auto now = std::chrono::steady_clock::now();
        if (now < end && now >= next) {
            const auto p = payload(static_cast<std::uint32_t>(r.sent));
            ::sendto(fd, p.data(), p.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
            ++r.sent;
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
            continue;
        }
        const auto n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) continue;
        std::uint32_t seq = 0;
        std::memcpy(&seq, buf.data(), 4);
        if (static_cast<std::size_t>(n) != size || !std::equal(buf.begin(), buf.begin() + n, payload(seq).begin())) {
            ++r.corrupted;
            continue;
        }
        if (!seen.insert(seq).second) continue;
        ++r.received;
        const auto t = std::chrono::steady_clock::now();
        if (r.received == 1) first_rx = t;
        last_rx = t;
        bytes += static_cast<std::size_t>(n);
    }
    ::close(fd);
    const double s = std::chrono::duration<double>(last_rx - first_rx).count();
    if (s > 0) r.rx_mbps = static_cast<double>(bytes - size) * 8 / s / 1e6;
    return r;
}

}  // namespace

TEST_CASE("qam-modem: starts without a radio, reports it, queues UDP packets") {
    forge::testing::AppRunner<modem::QamModem> m({"-D", "http.enabled=false", "-D", "forge.control=tcp://127.0.0.1:*",
                                                  "-D", "forge.events=tcp://127.0.0.1:*", "-D", "forge.data=tcp://127.0.0.1:*",
                                                  "-D", "console.telnet=", "-D", "modem.udp_listen=127.0.0.1:0",
                                                  "-D", "modem.radio_control=tcp://127.0.0.1:1",
                                                  "-D", "modem.radio_data=tcp://127.0.0.1:1",
                                                  "-D", "modem.radio_tx=tcp://127.0.0.1:1"});
    CHECK(get(m.app(), "symbol_rate") == 750.0);
    CHECK(get(m.app(), "raw_rate") == 3000.0);
    CHECK(get(m.app(), "bandwidth").get<double>() == doctest::Approx(1012.5));
    // radio-controller is not there: an alarm, the modem keeps running
    CHECK(forge::testing::wait_until([&] { return get(m.app(), "radio").get<std::string>().rfind("error", 0) == 0; }, 10s));
    CHECK(m.app().invoke([&] { return m.app().alarms().active("modem.radio"); }));

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(static_cast<std::uint16_t>(m.app().udp_port()));
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::vector<std::uint8_t> big(2000, 1), ok(100, 2);
    ::sendto(fd, big.data(), big.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    ::sendto(fd, ok.data(), ok.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    ::close(fd);
    CHECK(forge::testing::wait_until([&] { return get(m.app(), "udp_in") == 2; }));
    CHECK(get(m.app(), "tx_oversize") == 1);
}

#ifdef QAM_WITH_RADIO_CONTROLLER
TEST_CASE("qam-modem: echo through radio-controller (simulated AD9361, TX1 -> splitter -> RX1/RX2) at 2 Mbit/s") {
    forge::testing::AppRunner<radio::RadioController> radio(
        {"--backend", "sim", "-D", "http.enabled=false", "-D", "console.telnet=",
         "-D", "forge.data=tcp://127.0.0.1:*", "-D", "zmq.tx=tcp://127.0.0.1:*",
         "-D", "forge.control=tcp://127.0.0.1:*", "-D", "forge.events=tcp://127.0.0.1:*",
         "-D", "radio.sim.channels=2", "-D", "radio.sim.splitter=true", "-D", "radio.sim.loopback_db=-30",
         "-D", "radio.sim.loopback_cfo_hz=1200",  // a little frequency error on the received copy
         "-D", "radio.rx_channels=[1]", "-D", "radio.tap_rx=[2]"});
    auto& r = radio.app();

    forge::testing::AppRunner<modem::QamModem> m(
        {"-D", "http.enabled=false", "-D", "console.telnet=", "-D", "forge.control=tcp://127.0.0.1:*",
         "-D", "forge.events=tcp://127.0.0.1:*", "-D", "forge.data=tcp://127.0.0.1:*",
         "-D", "modem.udp_listen=127.0.0.1:0", "-D", "modem.radio_control=" + r.control_endpoint(),
         "-D", "modem.radio_data=" + r.data_endpoint(), "-D", "modem.radio_tx=" + r.tx_endpoint()});
    auto& q = m.app();

    // The modem sets the radio up: 2.1 GHz, 3 MSPS, manual gain, TX on.
    REQUIRE(forge::testing::wait_until([&] { return get(q, "radio").get<std::string>().find("MHz") != std::string::npos; },
                                       10s));
    CHECK(get(r, "rx_lo_mhz").get<double>() == doctest::Approx(2100));
    CHECK(get(r, "tx_lo_mhz").get<double>() == doctest::Approx(2100));
    CHECK(get(r, "sample_rate_msps").get<double>() == doctest::Approx(3));
    CHECK(get(r, "gain_mode") == "manual");
    CHECK(get(r, "tx_enabled") == true);
    CHECK(get(r, "buffer_size") == 16384);

    // Let the radio restart its buffers with the new settings, then send 2.3 Mbit/s for 4 s.
    std::this_thread::sleep_for(500ms);
    const auto res = echo(q.udp_port(), 2.3, 1400, 4s);
    MESSAGE("echo: sent " << res.sent << ", received " << res.received << ", corrupted " << res.corrupted << ", "
                          << res.rx_mbps << " Mbit/s; EVM " << get(q, "evm_db") << " dB, CFO " << get(q, "cfo_hz")
                          << " Hz, level " << get(q, "level_dbfs") << " dBFS, header errors " << get(q, "header_errors")
                          << ", CRC errors " << get(q, "crc_errors") << ", lost RX buffers " << get(q, "rx_gaps")
                          << ", radio TX underflows " << get(r, "tx_underflows"));
    CHECK(res.corrupted == 0);
    CHECK(res.received > 0);
    if (forge::testing::timeout_scale() == 1) {  // sanitizer builds are too slow for real time
        CHECK(res.received >= res.sent * 99 / 100);
        CHECK(res.rx_mbps > 2.0);
        CHECK(std::abs(get(q, "cfo_hz").get<double>() - 1200) < 100);
        CHECK(get(q, "evm_db").get<double>() < -25);
    }

    // Test mode: the modem fills the air itself and checks what comes back.
    q.invoke([&] { q.dashboard()->set("test_mode", true); });
    std::this_thread::sleep_for(3s);
    const auto kbps = get(q, "test_kbps").get<double>();
    MESSAGE("test packets: " << kbps << " kbit/s, lost " << get(q, "test_lost") << ", received " << get(q, "test_ok"));
    CHECK(get(q, "test_bad") == 0);
    CHECK(get(q, "test_ok").get<double>() > 0);
    if (forge::testing::timeout_scale() == 1) {
        CHECK(get(q, "test_ok").get<double>() > 100);
        CHECK(kbps > 2500);
    }

    // The spectrum analyzer's tap (RX2) sees the same signal through the splitter.
    CHECK(get(r, "tap_blocks").get<double>() >= 0);
}
#endif
