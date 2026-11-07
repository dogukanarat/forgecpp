#include "modem.hpp"

#include "radio/v1/radio.pb.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

using namespace std::chrono_literals;

namespace modem {
namespace {

constexpr const char* kRadioAlarm = "modem.radio";
constexpr const char* kNoRxAlarm = "modem.no_rx";
constexpr std::uint8_t kTestMagic[4] = {'Q', 'A', 'M', 'T'};

double round_to(double v, double step) { return std::round(v / step) * step; }

/// "host:port" (host may be empty or "*" = any).
sockaddr_in parse_address(const std::string& s, const char* what) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos) throw forge::ConfigError(std::string(what) + ": expected host:port, got '" + s + "'");
    std::string host = s.substr(0, colon);
    const int port = std::stoi(s.substr(colon + 1));
    if (port < 0 || port > 65535) throw forge::ConfigError(std::string(what) + ": bad port in '" + s + "'");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<std::uint16_t>(port));
    if (host.empty() || host == "*") host = "0.0.0.0";
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
        throw forge::ConfigError(std::string(what) + ": '" + host + "' is not an IPv4 address");
    }
    return a;
}

/// Test packet: magic, counter (big endian), bytes that follow from the counter.
void make_test_packet(std::vector<std::uint8_t>& p, std::uint32_t n, std::size_t size) {
    p.resize(std::max<std::size_t>(size, 8));
    std::memcpy(p.data(), kTestMagic, 4);
    for (int i = 0; i < 4; ++i) p[4 + i] = static_cast<std::uint8_t>(n >> (24 - 8 * i));
    std::uint32_t x = n * 2654435761u + 1;
    for (std::size_t i = 8; i < p.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = static_cast<std::uint8_t>(x);
    }
}

}  // namespace

QamModem::QamModem() : Application({"qam-modem", QAM_VERSION, "16QAM packet modem over an AD936x (radio-controller)"}) {}

// ---------------------------------------------------------------------------
// Configuration and controls

void QamModem::setup_config(forge::config::Schema& s) {
    s.section("modem", "16QAM modem");
    s.string("modem.radio_control", "tcp://127.0.0.1:5557", "radio-controller [forge].control");
    s.string("modem.radio_data", "tcp://127.0.0.1:5555", "radio-controller [forge].data (stream \"iq\")");
    s.string("modem.radio_tx", "tcp://127.0.0.1:5556", "radio-controller [zmq].tx");
    s.integer("modem.rx_channel", 1, "RX channel the modem demodulates (one of radio-controller's rx_channels)").range(1, 2);
    s.boolean("modem.configure_radio", true, "Set radio-controller up at start (LO, rate, bandwidth, gains, TX on)");
    s.number("modem.lo_hz", 2.1e9, "RX and TX LO").range(70e6, 6e9).unit("Hz");
    s.number("modem.sample_rate_hz", 3e6, "Sample rate (symbol rate = rate / sps)").range(2.084e6, 61.44e6).unit("Hz");
    s.number("modem.rf_bandwidth_hz", 2e6, "AD936x analog filter bandwidth, RX and TX").range(0.2e6, 56e6).unit("Hz");
    s.number("modem.tx_attenuation_db", 20, "TX attenuation").range(0, 89.75).unit("dB");
    s.number("modem.rx_gain_db", 30, "RX gain (manual: bursts confuse the AGC)").range(-3, 71).unit("dB");
    s.integer("modem.radio_buffer", 16384, "radio-controller buffer_size: smaller = less latency").range(1024, 1 << 20);
    s.integer("modem.sps", 4, "Samples per symbol").range(2, 16);
    s.number("modem.rolloff", 0.35, "RRC roll-off").range(0.1, 1.0);
    s.number("modem.tx_level_dbfs", -12, "TX signal RMS").range(-40, -6).unit("dBFS");
    s.number("modem.detect_threshold", 0.6, "Preamble detector threshold (Schmidl & Cox metric)").range(0.2, 0.95);
    s.integer("modem.max_payload", 1500, "Largest packet").range(16, 4000).unit("bytes");
    s.integer("modem.block_samples", 4096, "Samples per TX block sent to radio-controller").range(256, 1 << 18);
    s.integer("modem.lead_ms", 25, "How far TX runs ahead of RX (latency vs. underflows)").range(2, 1000).unit("ms");
    s.integer("modem.queue_packets", 64, "TX queue; more is dropped").range(1, 100000);
    s.string("modem.udp_listen", "0.0.0.0:6000", "Packets to transmit arrive here (UDP)");
    s.string("modem.udp_reply", "", "Received packets go here (host:port); empty = to the last sender");
    s.boolean("modem.test_mode", false, "Send numbered test packets when the TX queue is empty");
    s.integer("modem.test_size", 1400, "Test packet size").range(8, 4000).unit("bytes");
}

void QamModem::setup_web(forge::web::Dashboard& ui) {
    auto& link = ui.group("Link");
    link.value("radio", "Radio", [this] { return radio_status_; });
    link.toggle("tx_enabled", "Transmit", [this] { return tx_enabled_.load(); }, [this](bool on) { tx_enabled_ = on; });
    link.toggle("test_mode", "Test packets", [this] { return test_mode_.load(); }, [this](bool on) {
        test_mode_ = on;
        test_expect_ = -1;
    });
    link.number("tx_level_dbfs", "TX level", [this] { return tx_level_dbfs_.load(); },
                [this](double v) { tx_level_dbfs_ = std::clamp(v, -40.0, -6.0); })
        .range(-40, -6, 0.5)
        .unit("dBFS");
    link.value("symbol_rate", "Symbol rate", [this] { return params_.symbol_rate() / 1e3; }).unit("ksym/s");
    link.value("bandwidth", "Occupied bandwidth", [this] { return round_to(params_.occupied_bandwidth() / 1e3, 0.1); })
        .unit("kHz");
    link.value("raw_rate", "16QAM bit rate", [this] { return params_.symbol_rate() * 4 / 1e3; }).unit("kbit/s");
    link.button("configure_radio", "Configure radio", [this] { configure_radio(); });
    link.button("reset_stats", "Reset statistics", [this] { reset_stats(); });

    auto& tx = ui.group("Transmit");
    tx.value("udp_in", "From Ethernet", [this] { return udp_in_.packets.load(); });
    tx.value("udp_in_kbps", "Ethernet in", [this] { return round_to(udp_in_.kbps, 0.1); }).unit("kbit/s");
    tx.value("tx_packets", "Sent on air", [this] { return tx_sent_.packets.load(); });
    tx.value("tx_kbps", "Air rate", [this] { return round_to(tx_sent_.kbps, 0.1); }).unit("kbit/s");
    tx.value("tx_queue", "Queued", [this] {
        std::lock_guard lock(queue_mutex_);
        return queue_.size();
    });
    tx.value("tx_dropped", "Dropped (queue full)", [this] { return queue_dropped_.load(); });
    tx.value("tx_oversize", "Dropped (too large)", [this] { return oversize_.load(); });
    tx.value("tx_busy", "radio-controller busy", [this] { return tx_busy_.load(); });

    auto& rx = ui.group("Receive");
    rx.value("rx_packets", "Received", [this] { return rx_ok_.packets.load(); });
    rx.value("rx_kbps", "Rate", [this] { return round_to(rx_ok_.kbps, 0.1); }).unit("kbit/s");
    rx.value("evm_db", "EVM", [this] { return round_to(evm_db_.load(), 0.1); }).unit("dB");
    rx.value("snr_db", "SNR (from EVM)", [this] { return round_to(-evm_db_.load(), 0.1); }).unit("dB");
    rx.value("cfo_hz", "Frequency offset", [this] { return std::round(cfo_hz_.load()); }).unit("Hz");
    rx.value("level_dbfs", "Level", [this] { return round_to(level_db_.load(), 0.1); }).unit("dBFS");
    rx.value("detected", "Preambles", [this] { return demod_ ? demod_->stats().detected : 0; });
    rx.value("header_errors", "Header errors", [this] { return demod_ ? demod_->stats().header_errors : 0; });
    rx.value("crc_errors", "CRC errors", [this] { return demod_ ? demod_->stats().crc_errors : 0; });
    rx.value("rx_gaps", "Lost RX buffers", [this] { return rx_gaps_.load(); });
    rx.value("udp_out", "To Ethernet", [this] { return udp_out_.packets.load(); });

    auto& test = ui.group("Test");
    test.value("test_ok", "Received", [this] { return test_ok_.packets.load(); });
    test.value("test_kbps", "Rate", [this] { return round_to(test_ok_.kbps, 0.1); }).unit("kbit/s");
    test.value("test_lost", "Lost", [this] { return test_lost_.load(); });
    test.value("test_bad", "Wrong content", [this] { return test_bad_.load(); });
    test.value("test_per", "Packet error rate", [this] {
        const double ok = static_cast<double>(test_ok_.packets.load()), lost = static_cast<double>(test_lost_.load());
        return ok + lost > 0 ? lost / (ok + lost) : 0.0;
    }).precision(6);
}

void QamModem::setup_http(forge::http::Server& server) {
    Application::setup_http(server);
    // The latest payload symbols (thread-safe, off the main loop).
    server.get_json("/api/constellation", [this](const forge::http::Request&) {
        forge::json points = forge::json::array();
        if (demod_) {
            for (const auto& p : demod_->constellation()) {
                points.push_back({std::round(p.real() * 1000) / 1000, std::round(p.imag() * 1000) / 1000});
            }
        }
        return forge::json{{"points", points}, {"evm_db", evm_db_.load()}};
    });
}

// ---------------------------------------------------------------------------
// Start and stop

void QamModem::on_start() {
    const auto cfg = config();
    params_.sample_rate = cfg.value<double>("modem.sample_rate_hz");
    params_.sps = cfg.value<unsigned>("modem.sps");
    params_.rolloff = cfg.value<double>("modem.rolloff");
    params_.detect_threshold = cfg.value<double>("modem.detect_threshold");
    params_.max_payload = cfg.value<std::size_t>("modem.max_payload");
    tx_level_dbfs_ = cfg.value<double>("modem.tx_level_dbfs");
    params_.tx_rms = std::pow(10.0, tx_level_dbfs_ / 20);
    radio_control_ = cfg.value<std::string>("modem.radio_control");
    radio_data_ = cfg.value<std::string>("modem.radio_data");
    radio_tx_ = cfg.value<std::string>("modem.radio_tx");
    rx_channel_ = cfg.value<unsigned>("modem.rx_channel");
    block_samples_ = cfg.value<std::size_t>("modem.block_samples");
    lead_samples_ = static_cast<std::size_t>(cfg.value<double>("modem.lead_ms") * 1e-3 * params_.sample_rate);
    lead_samples_ = std::max(lead_samples_, 2 * block_samples_);
    queue_max_ = cfg.value<std::size_t>("modem.queue_packets");
    test_mode_ = cfg.value<bool>("modem.test_mode");
    test_size_ = std::min(cfg.value<std::size_t>("modem.test_size"), params_.max_payload);

    // UDP
    const auto listen = parse_address(cfg.value<std::string>("modem.udp_listen"), "modem.udp_listen");
    const auto reply = cfg.value<std::string>("modem.udp_reply");
    if (!reply.empty()) {
        peer_ = parse_address(reply, "modem.udp_reply");
        have_peer_ = fixed_peer_ = true;
    }
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd_ < 0) throw std::runtime_error(std::string("udp socket: ") + std::strerror(errno));
    const int big = 4 << 20;
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
    timeval tv{0, 200000};
    ::setsockopt(udp_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (::bind(udp_fd_, reinterpret_cast<const sockaddr*>(&listen), sizeof listen) != 0) {
        const std::string why = std::strerror(errno);
        ::close(udp_fd_);
        udp_fd_ = -1;
        throw std::runtime_error("udp bind " + cfg.value<std::string>("modem.udp_listen") + ": " + why);
    }
    sockaddr_in bound{};
    socklen_t len = sizeof bound;
    ::getsockname(udp_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
    udp_port_ = ntohs(bound.sin_port);

    mod_ = std::make_unique<qam::Modulator>(params_);
    demod_ = std::make_unique<qam::Demodulator>(params_);

    forge::zmq::SocketOptions so;
    so.send_hwm = 8;  // little queueing: the pacing below keeps latency low
    tx_out_ = std::make_unique<forge::zmq::Sender>(forge::zmq::Sender::Kind::push, radio_tx_, false, so);
    forge::zmq::Receiver::Options ro;
    ro.topics = {"iq"};
    rx_in_ = std::make_unique<forge::zmq::Receiver>(
        forge::zmq::Receiver::Kind::sub, radio_data_, [this](forge::zmq::Message&& m) { on_iq(std::move(m)); }, ro);

    credit_ = static_cast<std::int64_t>(lead_samples_);
    running_ = true;
    udp_thread_ = std::thread([this] { udp_loop(); });
    tx_thread_ = std::thread([this] { tx_loop(); });

    logger()->info("modem: {:.0f} ksym/s 16QAM ({:.2f} Mbit/s raw, {:.0f} kHz wide), UDP on port {}, radio {} / {} / {}",
                   params_.symbol_rate() / 1e3, params_.symbol_rate() * 4 / 1e6, params_.occupied_bandwidth() / 1e3,
                   udp_port_, radio_control_, radio_data_, radio_tx_);
    if (cfg.value<bool>("modem.configure_radio")) {
        configure_radio();
    } else {
        radio_status_ = "not configured by the modem (configure_radio = false)";
    }
    last_tick_ = std::chrono::steady_clock::now();
    every(1s, [this] { tick(); });
}

void QamModem::on_stop() {
    running_ = false;
    credit_cv_.notify_all();
    if (udp_thread_.joinable()) udp_thread_.join();
    if (tx_thread_.joinable()) tx_thread_.join();
    rx_in_.reset();  // stops the receiver thread before the demodulator goes
    tx_out_.reset();
    if (udp_fd_ >= 0) ::close(udp_fd_);
    udp_fd_ = -1;
}

// ---------------------------------------------------------------------------
// Radio set-up: forge.v1 requests to radio-controller, off the main loop

void QamModem::configure_radio() {
    if (configuring_) return;
    configuring_ = true;
    radio_status_ = "configuring";
    const auto cfg = config();
    const double lo_mhz = cfg.value<double>("modem.lo_hz") / 1e6;
    const double rate_msps = params_.sample_rate / 1e6;
    const double bw_mhz = cfg.value<double>("modem.rf_bandwidth_hz") / 1e6;
    const double atten = cfg.value<double>("modem.tx_attenuation_db");
    const double gain = cfg.value<double>("modem.rx_gain_db");
    const auto buffer = cfg.value<std::size_t>("modem.radio_buffer");
    const std::string endpoint = radio_control_;
    const unsigned ch = rx_channel_;
    background(
        [=]() -> std::string {
            try {
                forge::zmq::Client c(endpoint, 3s);
                c.set_client("qam-modem");
                c.set("rx_lo_mhz", lo_mhz);
                c.set("tx_lo_mhz", lo_mhz);
                c.set("sample_rate_msps", rate_msps);
                c.set("rx_bw_mhz", bw_mhz);
                c.set("tx_bw_mhz", bw_mhz);
                c.set(ch == 1 ? "gain_mode" : "rx2_gain_mode", "manual");
                c.set(ch == 1 ? "rx_gain_db" : "rx2_gain_db", gain);
                c.set("tx_atten_db", atten);
                if (c.get("buffer_size").get<double>() != static_cast<double>(buffer)) c.set("buffer_size", buffer);
                c.set("rx_enabled", true);
                c.set("tx_enabled", true);
                return {};
            } catch (const std::exception& e) {
                return e.what();
            }
        },
        [this, lo_mhz, rate_msps](std::string error) {
            configuring_ = false;
            radio_ok_ = error.empty();
            if (radio_ok_) {
                radio_status_ = fmt::format("{:.3f} MHz, {:.3f} MSPS", lo_mhz, rate_msps);
                alarms().clear(kRadioAlarm);
                logger()->info("modem: radio-controller set up ({})", radio_status_);
            } else {
                radio_status_ = "error: " + error;
                alarms().raise(kRadioAlarm, forge::Severity::error, "radio-controller set-up failed: " + error);
                logger()->warn("modem: radio-controller set-up failed: {} (retrying in 5 s)", error);
                after(5s, [this] {
                    if (!radio_ok_) configure_radio();
                });
            }
        });
}

// ---------------------------------------------------------------------------
// Statistics (main loop)

void QamModem::tick() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;
    for (Counter* c : {&udp_in_, &tx_sent_, &rx_ok_, &udp_out_, &test_ok_}) c->tick(dt);

    std::chrono::steady_clock::time_point last_rx;
    {
        std::lock_guard lock(credit_mutex_);
        last_rx = last_rx_block_;
    }
    const bool rx_missing = now - last_rx > 2s;
    alarms().set(kNoRxAlarm, rx_missing && radio_ok_, forge::Severity::warning,
                 fmt::format("no RX{} samples at {} MSPS from radio-controller ({}): is it streaming, is RX{} in its "
                             "rx_channels?",
                             rx_channel_, params_.sample_rate / 1e6, radio_data_, rx_channel_));
}

void QamModem::reset_stats() {
    for (Counter* c : {&udp_in_, &tx_sent_, &rx_ok_, &udp_out_, &test_ok_}) {
        c->packets = 0;
        c->bytes = 0;
        c->last_bytes = 0;
    }
    queue_dropped_ = oversize_ = rx_gaps_ = tx_busy_ = test_lost_ = test_bad_ = 0;
    test_expect_ = -1;
}

// ---------------------------------------------------------------------------
// UDP thread: Ethernet -> TX queue

void QamModem::udp_loop() {
    configure_thread("udp");
    std::vector<std::uint8_t> buf(65536);
    while (running_) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        const auto n = ::recvfrom(udp_fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
        if (n < 0) continue;  // timeout (or interrupted): check running_
        if (n == 0) continue;
        udp_in_.add(static_cast<std::size_t>(n));
        if (!fixed_peer_) {
            std::lock_guard lock(peer_mutex_);
            peer_ = from;
            have_peer_ = true;
        }
        if (static_cast<std::size_t>(n) > params_.max_payload) {
            ++oversize_;
            continue;
        }
        std::lock_guard lock(queue_mutex_);
        if (queue_.size() >= queue_max_) {
            ++queue_dropped_;
            continue;
        }
        queue_.emplace_back(buf.begin(), buf.begin() + n);
    }
}

// ---------------------------------------------------------------------------
// TX thread: queue -> modulator -> radio-controller, paced by the RX samples

bool QamModem::next_packet(std::vector<std::uint8_t>& payload, std::uint16_t& seq) {
    {
        std::lock_guard lock(queue_mutex_);
        if (!queue_.empty()) {
            payload = std::move(queue_.front());
            queue_.pop_front();
        }
    }
    if (payload.empty()) {
        if (!test_mode_) return false;
        make_test_packet(payload, test_counter_++, test_size_);
    }
    seq = seq_++;
    tx_sent_.add(payload.size());
    return true;
}

void QamModem::add_credit(std::size_t samples) {
    {
        std::lock_guard lock(credit_mutex_);
        credit_ = std::min<std::int64_t>(credit_ + static_cast<std::int64_t>(samples),
                                          static_cast<std::int64_t>(lead_samples_));
        last_rx_block_ = std::chrono::steady_clock::now();
    }
    credit_cv_.notify_one();
}

void QamModem::tx_loop() {
    configure_thread("tx");
    std::vector<std::int16_t> block(2 * block_samples_);
    radio::v1::TxBlock header;
    header.set_format("cs16");
    const auto n = static_cast<std::int64_t>(block_samples_);
    auto wall = std::chrono::steady_clock::now();
    double level = tx_level_dbfs_;

    while (running_) {
        {
            // Wait for credit: RX blocks bring it (TX and RX share the sample clock).
            // Without RX (radio not streaming yet) the wall clock does, so packets still go out.
            std::unique_lock lock(credit_mutex_);
            credit_cv_.wait_for(lock, 5ms, [&] { return !running_ || credit_ >= n; });
            const auto now = std::chrono::steady_clock::now();
            if (now - last_rx_block_ > 200ms) {
                const double add = std::chrono::duration<double>(now - wall).count() * params_.sample_rate;
                credit_ = std::min<std::int64_t>(credit_ + static_cast<std::int64_t>(add),
                                                 static_cast<std::int64_t>(lead_samples_));
            }
            wall = now;
            if (!running_ || credit_ < n) continue;
            credit_ -= n;
        }
        if (!tx_enabled_) continue;  // credit is spent, nothing goes out
        if (tx_level_dbfs_ != level && mod_->idle()) {  // new level: between packets
            level = tx_level_dbfs_;
            qam::Params p = params_;  // params_ is read by the controls: not changed here
            p.tx_rms = std::pow(10.0, level / 20);
            mod_ = std::make_unique<qam::Modulator>(p);
        }
        mod_->fill(block.data(), block_samples_,
                   [this](std::vector<std::uint8_t>& p, std::uint16_t& s) { return next_packet(p, s); });
        // Backpressure: radio-controller takes blocks as fast as the DAC plays them.
        zmq::pollitem_t item{static_cast<void*>(tx_out_->native()), 0, ZMQ_POLLOUT, 0};
        bool sent = false;
        while (running_ && !sent) {
            if (zmq::poll(&item, 1, std::chrono::milliseconds(50)) > 0 && (item.revents & ZMQ_POLLOUT)) {
                sent = tx_out_->publish("tx", header, block.data(), block.size() * sizeof(std::int16_t));
            }
            if (!sent) ++tx_busy_;
        }
        ++tx_blocks_;
    }
}

// ---------------------------------------------------------------------------
// RX (the Receiver thread): radio-controller -> demodulator -> Ethernet

void QamModem::on_iq(forge::zmq::Message&& m) {
    radio::v1::IqBlock b;
    try {
        b = m.as<radio::v1::IqBlock>();
    } catch (const forge::zmq::Error&) {
        return;
    }
    if (b.channel() != rx_channel_ || b.format() != "cs16" || m.blob.size() != std::size_t(b.samples()) * 4) return;
    rx_rate_hz_ = b.sample_rate();
    if (std::abs(b.sample_rate() - params_.sample_rate) > 1) return;  // not set up yet (or by someone else)
    ++rx_blocks_;
    const auto last = last_rx_buffer_.exchange(static_cast<std::int64_t>(b.buffer()));
    if (last >= 0 && b.buffer() != static_cast<std::uint64_t>(last) + 1) {
        ++rx_gaps_;  // samples lost: a packet across the gap cannot be decoded
        demod_->reset();
    }
    demod_->push(reinterpret_cast<const std::int16_t*>(m.blob.data()), b.samples(),
                 [this](qam::Packet&& p) { on_packet(std::move(p)); });
    add_credit(b.samples());
}

void QamModem::on_packet(qam::Packet&& p) {
    rx_ok_.add(p.payload.size());
    evm_db_ = p.evm_db;
    cfo_hz_ = p.cfo_hz;
    level_db_ = p.level_db;
    if (p.payload.size() >= 8 && std::memcmp(p.payload.data(), kTestMagic, 4) == 0) {
        std::uint32_t n = 0;
        for (int i = 0; i < 4; ++i) n = (n << 8) | p.payload[4 + i];
        std::vector<std::uint8_t> expect;
        make_test_packet(expect, n, p.payload.size());
        if (expect != p.payload) {
            ++test_bad_;
            return;
        }
        test_ok_.add(p.payload.size());
        const auto want = test_expect_.load();
        if (want >= 0 && static_cast<std::int32_t>(n) > want) test_lost_ += static_cast<std::uint32_t>(n) - static_cast<std::uint32_t>(want);
        test_expect_ = static_cast<std::int32_t>(n + 1);
        return;
    }
    sockaddr_in to{};
    {
        std::lock_guard lock(peer_mutex_);
        if (!have_peer_) return;
        to = peer_;
    }
    if (::sendto(udp_fd_, p.payload.data(), p.payload.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to) >= 0) {
        udp_out_.add(p.payload.size());
    }
}

}  // namespace modem
