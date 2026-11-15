// The parts below the application: the FPGA's words, the capture, the
// simulator and the protocol decoders.
#include "capture.hpp"
#include "decoders.hpp"
#include "sim.hpp"
#include "wire.hpp"

#include <forge/testing.hpp>

#include <string>

using namespace la;
using namespace std::chrono_literals;

namespace {

/// `seconds` of a source, as the engine would record it (no trigger).
Capture record(Source& src, double seconds, double rate = 100e6) {
    src.start({.sample_hz = rate});
    WireDecoder dec;
    Capture c;
    c.tick_hz = rate;
    std::vector<std::uint32_t> w;
    const auto end = static_cast<std::uint64_t>(seconds * rate);
    while (!dec.started() || dec.tick() < end) {
        w.clear();
        src.read(w, 0ms);
        dec.feed(w, [&](const WireEvent& e) {
            if (e.kind == WireEvent::start) {
                c.initial = e.levels;
            } else if (e.tick <= end) {
                c.push(e.tick, e.levels);
            }
        });
    }
    c.end = end;
    c.done = true;
    return c;
}

std::string text(const std::vector<DecodedFrame>& frames) {
    std::string s;
    for (const auto& f : frames) s += static_cast<char>(f.value);
    return s;
}

/// A capture made of UART bytes on channel 0.
Capture uart_capture(const std::vector<std::uint8_t>& bytes, double baud, const std::string& parity = "none",
                     double rate = 10e6) {
    std::vector<std::pair<double, std::pair<unsigned, bool>>> edges;
    uart_edges(0.001, baud, bytes, 0, edges, 8, parity);
    Capture c;
    c.tick_hz = rate;
    c.initial = 1;
    for (const auto& [t, e] : edges) c.push(static_cast<std::uint64_t>(t * rate), e.second ? 1 : 0);
    c.end = static_cast<std::uint64_t>(0.1 * rate);
    return c;
}

}  // namespace

TEST_CASE("wire: words round trip, long idle times and gaps") {
    std::vector<std::uint32_t> words;
    WireEncoder enc(words);
    enc.start(0b0101);
    enc.change(10, 0b0100);
    enc.change(10, 0b0100);              // no change: no word
    enc.time(1000);                       // heartbeat
    enc.change(3ull * kMaxDelta, 0b1100); // needs time words in between
    enc.gap(3ull * kMaxDelta + 50, 0b0000);
    enc.change(3ull * kMaxDelta + 60, 0b0001);

    std::vector<WireEvent> ev;
    WireDecoder dec;
    std::vector<std::uint32_t> noise{make_word(WordType::edge, 3, 7)};  // before the start word: skipped
    dec.feed(noise, [&](const WireEvent& e) { ev.push_back(e); });
    dec.feed(words, [&](const WireEvent& e) {
        if (e.kind != WireEvent::time) ev.push_back(e);
    });
    CHECK(dec.skipped() == 1);
    REQUIRE(ev.size() == 5);
    CHECK(ev[0].kind == WireEvent::start);
    CHECK(ev[0].levels == 0b0101);
    CHECK(ev[1].tick == 10);
    CHECK(ev[1].levels == 0b0100);
    CHECK(ev[2].tick == 3ull * kMaxDelta);
    CHECK(ev[2].levels == 0b1100);
    CHECK(ev[3].kind == WireEvent::gap);
    CHECK(ev[3].tick == 3ull * kMaxDelta + 50);
    CHECK(ev[4].levels == 0b0001);
    CHECK(dec.gaps() == 1);
    for (const auto w : words) CHECK(word_delta(w) <= kMaxDelta);
}

TEST_CASE("capture: changes, levels, counts, front removal") {
    Capture c;
    c.initial = 0b0001;
    c.push(10, 0b0011);
    c.push(20, 0b0011);  // nothing changes
    c.push(30, 0b0010);
    c.push(30, 0b0110);  // same tick: the last state wins
    c.push(40, 0b0111);
    c.end = 100;
    REQUIRE(c.size() == 3);
    CHECK(c.levels_at(0) == 0b0001);
    CHECK(c.levels_at(10) == 0b0011);
    CHECK(c.levels_at(35) == 0b0110);
    CHECK(c.level(2, 31));
    CHECK(c.count(0, 0, 100) == 2);
    CHECK(c.count(1, 0, 100) == 1);
    Cursor cur(c);
    CHECK(cur.at(5) == 0b0001);
    CHECK(cur.at(45) == 0b0111);
    CHECK(cur.at(15) == 0b0011);  // backwards works too
    CHECK(*cur.next_edge(2, 0) == 30);

    c.drop_before(35);
    CHECK(c.begin == 35);
    CHECK(c.initial == 0b0110);
    REQUIRE(c.size() == 1);
    CHECK(c.t[0] == 40);
}

TEST_CASE("capture: a view keeps single edges and merges what a pixel cannot show") {
    Capture c;
    c.tick_hz = 1e6;
    // channel 0: a burst of 100 edges in ticks 1000..1099, then one edge at 5000
    std::uint8_t lv = 0;
    for (int i = 0; i < 100; ++i) {
        lv ^= 1;
        c.push(1000 + static_cast<std::uint64_t>(i), lv);
    }
    c.push(5000, lv ^ 1);
    c.end = 10000;
    const View v = make_view(c, 0, 9999, 100);  // 100 ticks per pixel
    const auto& items = v.channels[0].items;
    REQUIRE(items.size() == 2);
    CHECK(items[0].count == 100);
    CHECK(items[0].t0 == 1000);
    CHECK(items[0].t1 == 1099);
    CHECK(items[1].count == 1);
    CHECK(items[1].t0 == 5000);
    CHECK(items[1].level == true);
    // zoomed in, every edge is its own
    const View z = make_view(c, 1000, 1009, 100);
    CHECK(z.channels[0].initial == true);  // the edge at 1000 is the starting level
    CHECK(z.channels[0].items.size() == 9);
    CHECK(z.channels[1].items.empty());
}

TEST_CASE("capture: frequency, duty and pulse widths of a PWM signal") {
    Capture c;
    c.tick_hz = 1e6;
    for (std::uint64_t k = 0; k < 10; ++k) {  // 1 kHz, 25 % duty
        c.push(1000 + k * 1000, 1);
        c.push(1250 + k * 1000, 0);
    }
    c.end = 12000;
    const auto s = measure(c, 0, 1000, 11000);
    CHECK(s.rising == 10);
    CHECK(s.falling == 10);
    REQUIRE(s.frequency_hz());
    CHECK(*s.frequency_hz() == doctest::Approx(1000));
    CHECK(*s.duty() == doctest::Approx(0.25));
    CHECK(*s.width_high_min_s == doctest::Approx(250e-6));
    CHECK(*s.width_low_max_s == doctest::Approx(750e-6));
}

TEST_CASE("decoders: UART text, parity and framing errors, a break") {
    const std::string hello = "Hello\r\n";
    auto c = uart_capture({hello.begin(), hello.end()}, 115200);
    auto dec = make_decoder("uart", normalize_settings("uart", {{"channel", 0}, {"baud", 115200}}));
    auto frames = dec->decode(c);
    CHECK(text(frames) == hello);
    for (const auto& f : frames) CHECK(f.flags == 0);

    // Sent with even parity, read as odd: every byte has a parity error.
    c = uart_capture({0x41, 0x42}, 9600, "even");
    frames = make_decoder("uart", normalize_settings("uart", {{"baud", 9600}, {"parity", "odd"}}))->decode(c);
    REQUIRE(frames.size() == 2);
    CHECK((frames[0].flags & kParityError));
    frames = make_decoder("uart", normalize_settings("uart", {{"baud", 9600}, {"parity", "even"}}))->decode(c);
    CHECK(frames[0].flags == 0);
    CHECK(frames[1].value == 0x42);

    // A line held low for longer than a frame: a break (framing error, all zeros).
    Capture b;
    b.tick_hz = 1e6;
    b.initial = 1;
    b.push(1000, 0);
    b.push(1000 + 5000, 1);
    b.end = 20000;
    frames = make_decoder("uart", normalize_settings("uart", {{"baud", 9600}}))->decode(b);
    REQUIRE(!frames.empty());
    CHECK((frames[0].flags & kBreak));
    CHECK(describe("uart", frames[0], "hex") == "Break");
}

TEST_CASE("sim mixed: the UART text, the I2C sensor reads and the PWM decode as sent") {
    SimSource src(make_scenario("mixed"), false);
    const Capture c = record(src, 0.05);

    const auto uart = make_decoder("uart", normalize_settings("uart", {{"channel", 0}, {"baud", 115200}}))->decode(c);
    CHECK(text(uart).starts_with("Hello from Arty Z7! #0\r\nHello from Arty Z7! #1\r\n"));

    const auto i2c = make_decoder("i2c", normalize_settings("i2c", {{"scl", 1}, {"sda", 2}}))->decode(c);
    REQUIRE(i2c.size() >= 9);
    CHECK(i2c[0].kind == FrameKind::start);
    CHECK(i2c[1].kind == FrameKind::address);
    CHECK(i2c[1].value == 0x48);
    CHECK_FALSE((i2c[1].flags & kRead));
    CHECK_FALSE((i2c[1].flags & kNack));
    CHECK(i2c[2].value == 0x00);
    CHECK(i2c[3].kind == FrameKind::start);
    CHECK((i2c[3].flags & kRepeated));
    CHECK(i2c[4].value == 0x48);
    CHECK((i2c[4].flags & kRead));
    CHECK(i2c[5].value == 24);  // 24.0 °C, high byte
    CHECK_FALSE((i2c[5].flags & kNack));
    CHECK((i2c[6].flags & kNack));  // the master NACKs the last byte
    CHECK(i2c[7].kind == FrameKind::stop);
    CHECK(describe("i2c", i2c[4], "hex") == "Read from 0x48 ACK");
    // the fifth transaction: nobody at 0x50
    bool absent = false;
    for (const auto& f : i2c) absent |= f.kind == FrameKind::address && f.value == 0x50 && (f.flags & kNack);
    CHECK(absent);

    const auto pwm = measure(c, 3, 0, c.end);
    REQUIRE(pwm.frequency_hz());
    CHECK(*pwm.frequency_hz() == doctest::Approx(1000).epsilon(0.001));
}

TEST_CASE("sim spi: a flash's JEDEC ID and a data read") {
    SimSource src(make_scenario("spi"), false);
    const Capture c = record(src, 0.012);
    const auto spi = make_decoder("spi", normalize_settings("spi", {}))->decode(c);
    REQUIRE(spi.size() >= 12);
    CHECK(spi[0].value == 0x9F);
    CHECK(spi[1].value2 == 0xEF);
    CHECK(spi[2].value2 == 0x40);
    CHECK(spi[3].value2 == 0x18);
    CHECK(spi[4].value == 0x03);  // read at address 16
    CHECK(spi[7].value == 0x10);
    CHECK(spi[8].value2 == ((16 * 7 + 0x30) & 0xFF));
    CHECK(describe("spi", spi[0], "hex") == "MOSI 0x9F  MISO 0xFF");
    for (const auto& f : spi) CHECK(f.kind == FrameKind::data);
    // At 1 MHz sampling (one sample per SPI bit) the clock cannot be seen: nothing decodes cleanly.
    SimSource slow(make_scenario("spi"), false);
    const Capture s = record(slow, 0.012, 1e6);
    CHECK(make_decoder("spi", normalize_settings("spi", {}))->decode(s).size() < spi.size());
}

TEST_CASE("sim uart: command and answer on two lines at 9600 baud") {
    SimSource src(make_scenario("uart"), false);
    const Capture c = record(src, 0.25, 1e6);
    const auto tx = make_decoder("uart", normalize_settings("uart", {{"channel", 0}, {"baud", 9600}}))->decode(c);
    const auto rx = make_decoder("uart", normalize_settings("uart", {{"channel", 1}, {"baud", 9600}}))->decode(c);
    CHECK(text(tx) == "AT+TEMP?\r\nAT+TEMP?\r\n");  // at 2 ms and 202 ms
    CHECK(text(rx).starts_with("+TEMP: 24.0\r\nOK\r\n"));
}

TEST_CASE("decoders: settings are checked") {
    CHECK_THROWS_AS(normalize_settings("can", {}), std::invalid_argument);
    CHECK_THROWS_AS(normalize_settings("uart", {{"channel", 4}}), std::invalid_argument);
    CHECK_THROWS_AS(normalize_settings("uart", {{"baud", 1.5}}), std::invalid_argument);
    CHECK_THROWS_AS(normalize_settings("uart", {{"parity", "mark"}}), std::invalid_argument);
    CHECK_THROWS_AS(normalize_settings("i2c", {{"scl", 2}, {"sda", 2}}), std::invalid_argument);
    CHECK_THROWS_AS(normalize_settings("spi", {{"mosi", -1}, {"miso", -1}}), std::invalid_argument);
    const auto s = normalize_settings("uart", {{"baud", 9600}});
    CHECK(s["bits"] == 8);
    CHECK(s["parity"] == "none");
    CHECK(format_value('A', 8, "ascii") == "'A'");
    CHECK(format_value('\n', 8, "ascii") == "'\\n'");
    CHECK(format_value(5, 8, "bin") == "0b00000101");
    CHECK(format_value(0x1234, 16, "hex") == "0x1234");
}
