#pragma once
// qam-modem: Ethernet <-> 16QAM over an AD936x, through radio-controller.
//
//   UDP [modem].udp_listen --> TX queue --> Modulator --PUSH TxBlock--> radio-controller [zmq].tx
//                                                                         | TX1 -> cable/splitter -> RX1
//   UDP reply  <-- packets <-- Demodulator <--SUB "iq" (RX1)------------- radio-controller [forge].data
//
// Threads (docs/threading.md):
//   udp   blocking recvfrom -> TX queue (mutex)
//   tx    modulates blocks as fast as RX samples come in (the same clock: TX and
//         RX of one AD936x), PUSH with backpressure; idle = zeros
//   rx    the Receiver thread: demodulates every RX block, sends packets out by UDP
//   main  controls, statistics, radio set-up (background)
// Thread-shared values are atomics or behind mutexes; the controls read those.
//
// Test mode: without any UDP traffic the modem sends numbered test packets
// back-to-back and counts what comes back (lost / received / rate).

#include "qam.hpp"

#include <forge/forge.hpp>
#include <forge/zmq.hpp>

#include <netinet/in.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace modem {

class QamModem : public forge::Application {
public:
    QamModem();

    /// Bound UDP port (after start; useful with udp_listen port 0 in tests).
    unsigned udp_port() const { return udp_port_; }

protected:
    void setup_config(forge::config::Schema& s) override;
    void setup_web(forge::web::Dashboard& ui) override;
    void setup_http(forge::http::Server& server) override;
    void on_start() override;
    void on_stop() override;

private:
    struct Counter {  // packets and bytes, rate per second (main loop computes)
        std::atomic<std::uint64_t> packets{0}, bytes{0};
        std::uint64_t last_bytes = 0;
        double kbps = 0;
        void add(std::size_t n) {
            ++packets;
            bytes += n;
        }
        void tick(double dt) {
            const auto b = bytes.load();
            kbps = dt > 0 ? static_cast<double>(b - last_bytes) * 8 / dt / 1e3 : 0;
            last_bytes = b;
        }
    };

    // radio set-up (background) and statistics (main loop)
    void configure_radio();
    void tick();
    void reset_stats();

    // threads
    void udp_loop();
    void tx_loop();
    void on_iq(forge::zmq::Message&& m);
    void on_packet(qam::Packet&& p);
    bool next_packet(std::vector<std::uint8_t>& payload, std::uint16_t& seq);
    void add_credit(std::size_t samples);

    // configuration (fixed after start)
    qam::Params params_;
    std::string radio_control_, radio_data_, radio_tx_;
    unsigned rx_channel_ = 1;
    std::size_t block_samples_ = 4096;
    std::size_t lead_samples_ = 0;
    std::size_t queue_max_ = 64;
    std::size_t test_size_ = 1400;

    // udp (socket: udp thread receives, rx thread sends)
    int udp_fd_ = -1;
    unsigned udp_port_ = 0;
    std::mutex peer_mutex_;
    sockaddr_in peer_{};
    bool have_peer_ = false, fixed_peer_ = false;

    // TX queue: udp thread -> tx thread
    std::mutex queue_mutex_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::uint16_t seq_ = 0;  // tx thread
    std::uint32_t test_counter_ = 0;  // tx thread

    // pacing: credit = samples the tx thread may send; RX blocks add to it
    std::mutex credit_mutex_;
    std::condition_variable credit_cv_;
    std::int64_t credit_ = 0;
    std::chrono::steady_clock::time_point last_rx_block_{};

    std::unique_ptr<qam::Modulator> mod_;      // tx thread
    std::unique_ptr<qam::Demodulator> demod_;  // rx (Receiver) thread; stats()/constellation() anywhere
    std::unique_ptr<forge::zmq::Sender> tx_out_;  // tx thread
    std::unique_ptr<forge::zmq::Receiver> rx_in_;
    std::thread udp_thread_, tx_thread_;
    std::atomic<bool> running_{false};

    // shared settings (controls write, threads read)
    std::atomic<bool> tx_enabled_{true}, test_mode_{false};
    std::atomic<double> tx_level_dbfs_{-12};

    // statistics (threads write atomics, the main loop reads)
    Counter udp_in_, tx_sent_, rx_ok_, udp_out_, test_ok_;
    std::atomic<std::uint64_t> queue_dropped_{0}, oversize_{0}, rx_gaps_{0}, tx_busy_{0}, test_lost_{0}, test_bad_{0},
        rx_blocks_{0}, tx_blocks_{0};
    std::atomic<std::int64_t> last_rx_buffer_{-1};
    std::atomic<std::uint64_t> rx_late_{0};
    std::uint64_t last_rx_late_ = 0;  // main loop
    std::int64_t max_lag_ns_ = 200000000;
    bool skipping_ = false;           // receiver thread
    std::atomic<std::int32_t> test_expect_{-1};
    std::atomic<float> evm_db_{0}, cfo_hz_{0}, level_db_{-200};
    std::atomic<double> rx_rate_hz_{0};
    std::chrono::steady_clock::time_point last_tick_{};

    // main loop
    bool radio_ok_ = false;
    std::string radio_status_ = "not configured";
    bool configuring_ = false;
};

}  // namespace modem
