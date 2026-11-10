// channel-emulator as an application: blocks in, impaired blocks out; and with
// the 16QAM modem: packet error rate per preset, and the whole chain
// qam-modem -> channel-emulator -> radio-controller (simulated) -> qam-modem.
#include "emulator.hpp"

#include "radio/radio.forge.hpp"

#include <forge/testing.hpp>

#ifdef EMU_WITH_MODEM
#include "modem.hpp"
#include "qam.hpp"
#include "radio_controller.hpp"
#endif

#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <thread>

using namespace std::chrono_literals;
using forge::json;
namespace fz = forge::zmq;

namespace {

// Command line arguments: "-D <setting>" for each setting, after `first`.
std::vector<std::string> settings(std::initializer_list<std::string> list, std::vector<std::string> first = {}) {
    for (const auto& s : list) {
        first.push_back("-D");
        first.push_back(s);
    }
    return first;
}

json get(forge::Application& app, const std::string& id) {
    return app.invoke([&] { return app.dashboard()->get(id); });
}
void set(forge::Application& app, const std::string& id, const json& v) {
    app.invoke([&] { app.dashboard()->set(id, v); });
}

/// Stands in for radio-controller's TX input: collects the blocks.
struct Sink {
    std::mutex m;
    std::deque<std::pair<radio_tx_block, std::string>> blocks;
    fz::Receiver rx;
    Sink()
        : rx(fz::Receiver::Kind::pull, "tcp://127.0.0.1:*", [this](fz::Message&& msg) {
              std::lock_guard lock(m);
              blocks.emplace_back(msg.as<radio_tx_block>(), std::move(msg.blob));
          }, fz::Receiver::Options{true, {""}, {}}) {}
    std::size_t size() {
        std::lock_guard lock(m);
        return blocks.size();
    }
    std::pair<radio_tx_block, std::string> pop() {
        std::lock_guard lock(m);
        auto b = std::move(blocks.front());
        blocks.pop_front();
        return b;
    }
};

std::string tone_blob(std::size_t n) {
    std::string b(n * 4, '\0');
    auto* iq = reinterpret_cast<std::int16_t*>(b.data());
    for (std::size_t i = 0; i < n; ++i) {
        iq[2 * i] = static_cast<std::int16_t>(8000 * std::cos(0.05 * static_cast<double>(i)));
        iq[2 * i + 1] = static_cast<std::int16_t>(8000 * std::sin(0.05 * static_cast<double>(i)));
    }
    return b;
}

}  // namespace

TEST_CASE("channel-emulator: forwards every block, impaired as the controls say") {
    Sink sink;
    forge::testing::AppRunner<emulator::ChannelEmulator> emu(
        settings({"http.enabled=false", "console.telnet=", "forge.control=tcp://127.0.0.1:*",
                  "forge.events=tcp://127.0.0.1:*", "forge.data=tcp://127.0.0.1:*", "emulator.input=tcp://127.0.0.1:*",
                  "emulator.output=" + sink.rx.endpoint(), "emulator.radio_control=", "emulator.sample_rate_hz=1e6"}));
    auto& e = emu.app();
    CHECK(get(e, "preset") == "clean");
    CHECK(get(e, "sample_rate") == 1.0);

    fz::Sender tx(fz::Sender::Kind::push, e.input_endpoint(), false);
    radio_tx_block h{};
    std::strcpy(h.format, "cs16");
    const auto blob = tone_blob(4096);
    auto send = [&] {
        REQUIRE(forge::testing::wait_until([&] { return tx.publish("tx", h, blob.data(), blob.size()); }));
    };

    // clean: bit exact
    send();
    REQUIRE(forge::testing::wait_until([&] { return sink.size() == 1; }));
    CHECK(sink.pop().second == blob);

    // an impairment: same number of samples, different samples, right power
    set(e, "frequency", true);
    set(e, "cfo_hz", 1000.0);
    CHECK(get(e, "preset") == "custom");
    send();
    REQUIRE(forge::testing::wait_until([&] { return sink.size() == 1; }));
    auto [hdr, out] = sink.pop();
    CHECK(std::string_view(hdr.format) == "cs16");
    CHECK(out.size() == blob.size());
    CHECK(out != blob);

    // presets, bypass, cyclic frames pass as they are
    set(e, "preset", "awgn-10");
    CHECK(get(e, "noise") == true);
    CHECK(get(e, "snr_db") == 10.0);
    CHECK(get(e, "frequency") == false);
    set(e, "bypass", true);
    send();
    REQUIRE(forge::testing::wait_until([&] { return sink.size() == 1; }));
    CHECK(sink.pop().second == blob);
    set(e, "bypass", false);
    h.cyclic = true;
    send();
    REQUIRE(forge::testing::wait_until([&] { return sink.size() == 1; }));
    CHECK(sink.pop().second == blob);
    CHECK(get(e, "cyclic") == 1);
    CHECK(get(e, "blocks") == 4);
    CHECK(get(e, "signal_dbfs").get<double>() == doctest::Approx(20 * std::log10(8000.0 / 32768)).epsilon(0.01));

    // multipath text control: parsed, normalized; nonsense refused
    set(e, "paths", "0:0, 0.5:-3:90");
    CHECK(get(e, "paths") == "0:0:0, 0.5:-3:90");
    CHECK_THROWS(set(e, "paths", "x:y"));
    CHECK(get(e, "paths") == "0:0:0, 0.5:-3:90");

    // no radio-controller downstream: the emulator waits (backpressure), it does not drop
    h.cyclic = false;
}

TEST_CASE("channel-emulator: path list parsing") {
    const auto p = emulator::parse_paths(" 0:0:0 ,1.5:-6 , 3:-10:45");
    REQUIRE(p.size() == 3);
    CHECK(p[1].delay_us == 1.5);
    CHECK(p[1].gain_db == -6);
    CHECK(p[2].phase_deg == 45);
    CHECK(emulator::format_paths(p) == "0:0:0, 1.5:-6:0, 3:-10:45");
    CHECK(emulator::parse_paths("").empty());
    CHECK_THROWS_AS(emulator::parse_paths("-1:0"), std::invalid_argument);
    CHECK_THROWS_AS(emulator::parse_paths("100:0"), std::invalid_argument);
}

#ifdef EMU_WITH_MODEM
TEST_CASE("channel-emulator: 16QAM modem packet error rate per preset") {
    // The modem's waveform through each preset (DSP only, no applications).
    const qam::Params qp;  // 3 MSPS, 750 ksym/s
    struct Result {
        std::size_t sent = 0, ok = 0;
        float evm = 0;
        qam::Stats st;
    };
    auto run = [&](channel::Settings s) {
        s.noise_bandwidth_hz = qp.symbol_rate();  // SNR = Es/N0
        channel::Channel ch(qp.sample_rate, 3);
        ch.configure(s);
        qam::Modulator mod(qp);
        qam::Demodulator demod(qp);
        Result r;
        const std::size_t packets = 60, len = 1000;
        std::uint16_t next = 0;
        std::vector<std::int16_t> block(2 * 4096);
        double evm = 0;
        std::size_t idle = 0, blocks = 0;
        while (idle < 20) {
            ++blocks;
            mod.fill(block.data(), 4096, [&](std::vector<std::uint8_t>& p, std::uint16_t& seq) {
                if (blocks < 3 || next == packets) return false;  // a little silence first
                p.resize(len);
                std::mt19937 g(next);
                for (auto& b : p) b = static_cast<std::uint8_t>(g());
                seq = next++;
                return true;
            });
            if (next == packets && mod.idle()) ++idle;
            ch.process(block.data(), 4096);
            demod.push(block.data(), 4096, [&](qam::Packet&& pkt) {
                std::mt19937 g(pkt.seq);
                std::vector<std::uint8_t> want(len);
                for (auto& b : want) b = static_cast<std::uint8_t>(g());
                REQUIRE(pkt.payload == want);  // never a corrupted packet
                ++r.ok;
                evm += pkt.evm_db;
            });
        }
        r.sent = packets;
        r.evm = r.ok ? static_cast<float>(evm / static_cast<double>(r.ok)) : 0;
        r.st = demod.stats();
        return r;
    };

    std::map<std::string, Result> res;
    std::string table = "\npreset          delivered  EVM dB  CRC err  hdr err\n";
    for (const auto& name : channel::preset_names()) {
        const auto r = run(channel::preset(name));
        res[name] = r;
        table += std::format("{:<14} {:>5}/{:<4} {:>7.1f} {:>8} {:>8}\n", name, r.ok, r.sent, r.evm, r.st.crc_errors,
                             r.st.header_errors);
    }
    MESSAGE(table);
    for (const char* good :
         {"clean", "awgn-30", "awgn-25", "offset-5k", "doppler", "oscillators", "indoor", "urban", "interference"}) {
        CAPTURE(good);
        CHECK(res[good].ok >= 58);
    }
    CHECK(res["clean"].ok == 60);
    CHECK(res["awgn-30"].evm == doctest::Approx(-30).epsilon(0.07));  // the SNR asked for is the SNR measured
    CHECK(res["awgn-20"].evm == doctest::Approx(-20).epsilon(0.07));
    CHECK(res["awgn-15"].ok < 10);  // 16QAM needs ~20 dB for 1000-byte packets
    CHECK(res["mobile"].ok >= 50);  // fading dips lower the SNR now and then; the tracking holds (see test_qam)
}

TEST_CASE("channel-emulator: qam-modem -> channel-emulator -> radio-controller (simulated) -> qam-modem") {
    forge::testing::AppRunner<radio::RadioController> radio(
        settings({"http.enabled=false", "console.telnet=", "forge.data=tcp://127.0.0.1:*", "zmq.tx=tcp://127.0.0.1:*",
                  "forge.control=tcp://127.0.0.1:*", "forge.events=tcp://127.0.0.1:*", "radio.sim.channels=2",
                  "radio.sim.splitter=true", "radio.sim.loopback_db=-30", "radio.rx_channels=[1]", "radio.tap_rx=[2]"},
                 {"--backend", "sim"}));
    auto& r = radio.app();
    forge::testing::AppRunner<emulator::ChannelEmulator> emu(
        settings({"http.enabled=false", "console.telnet=", "forge.control=tcp://127.0.0.1:*",
                  "forge.events=tcp://127.0.0.1:*", "forge.data=tcp://127.0.0.1:*", "emulator.input=tcp://127.0.0.1:*",
                  "emulator.output=" + r.tx_endpoint(), "emulator.radio_control=" + r.control_endpoint()}));
    auto& e = emu.app();
    forge::testing::AppRunner<modem::QamModem> m(settings(
        {"http.enabled=false", "console.telnet=", "forge.control=tcp://127.0.0.1:*", "forge.events=tcp://127.0.0.1:*",
         "forge.data=tcp://127.0.0.1:*", "modem.udp_listen=127.0.0.1:0", "modem.radio_control=" + r.control_endpoint(),
         "modem.radio_data=" + r.data_endpoint(), "modem.radio_tx=" + e.input_endpoint(), "modem.test_mode=true"}));
    auto& q = m.app();
    REQUIRE(forge::testing::wait_until(
        [&] { return get(q, "radio").get<std::string>().find("MHz") != std::string::npos; }, 10s));
    // the emulator reads the rate the modem set on the radio
    REQUIRE(forge::testing::wait_until([&] { return get(e, "sample_rate").get<double>() == 3.0; }, 10s));

    auto measure = [&](std::chrono::milliseconds d) {
        set(q, "reset_stats", true);
        std::this_thread::sleep_for(d);
        return std::make_tuple(get(q, "test_ok").get<double>(), get(q, "crc_errors").get<double>(),
                               get(q, "evm_db").get<double>());
    };
    const bool realtime = forge::testing::realtime();

    std::this_thread::sleep_for(1s);
    auto [ok_clean, crc_clean, evm_clean] = measure(2s);
    MESSAGE("clean: " << ok_clean << " test packets, EVM " << evm_clean << " dB");
    CHECK(ok_clean > 0);
    if (realtime)
        CHECK(ok_clean > 300);  // ~245 packets/s at 1400 bytes (sanitizer builds skip what they cannot keep up with)

    set(e, "preset", "awgn-20");
    std::this_thread::sleep_for(300ms);
    auto [ok20, crc20, evm20] = measure(2s);
    MESSAGE("awgn-20: " << ok20 << " test packets, " << crc20 << " CRC errors, EVM " << evm20 << " dB, emulator signal "
                        << get(e, "signal_dbfs") << " dBFS, CPU " << get(e, "cpu_percent") << " %");
    if (realtime) CHECK(ok20 > 0);
    if (realtime) CHECK(evm20 == doctest::Approx(-20).epsilon(0.1));  // Es/N0 set = measured at the modem

    set(e, "preset", "awgn-10");
    std::this_thread::sleep_for(300ms);
    auto [ok10, crc10, evm10] = measure(1500ms);
    MESSAGE("awgn-10: " << ok10 << " test packets, " << crc10 << " CRC errors");
    CHECK(ok10 == 0);
    CHECK(get(q, "test_bad") == 0);  // what does come through is never wrong

    set(e, "bypass", true);
    std::this_thread::sleep_for(300ms);
    auto [ok_back, crc_back, evm_back] = measure(1500ms);
    CHECK(ok_back > 0);
    if (realtime) CHECK(ok_back > 30);
    CHECK(get(e, "clipped") == 0);
}
#endif
