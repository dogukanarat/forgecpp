#include "forge/telnet.hpp"

#include "forge/console.hpp"
#include "forge/log.hpp"

#include <format>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <deque>
#include <stdexcept>

namespace forge {
namespace {

constexpr unsigned char IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240, IP = 244,
                        ECHO_OPT = 1;

std::string crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

}  // namespace

struct Telnet::Session {
    int fd = -1;
    std::string peer;
    std::mutex write_mutex;
    std::atomic<bool> open{true};
    Subscription log_listener;
    std::atomic<bool> color{true};  // ANSI level colours in followed log lines (`color on|off`)
    std::thread thread;
    std::chrono::steady_clock::time_point last_input = std::chrono::steady_clock::now();

    void send(std::string_view data) {
        std::lock_guard lock(write_mutex);
        std::size_t off = 0;
        while (open && off < data.size()) {
            const auto n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                open = false;
                return;
            }
            off += static_cast<std::size_t>(n);
        }
    }
    void print(std::string_view text) { send(crlf(text)); }

    // Telnet protocol state for incoming bytes.
    enum class In { data, iac, option, sub, sub_iac } in = In::data;
    std::string line;
    bool interrupted = false;

    /// Feed bytes; returns complete lines.
    std::vector<std::string> feed(const char* data, std::size_t n) {
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < n; ++i) {
            const auto c = static_cast<unsigned char>(data[i]);
            switch (in) {
                case In::data:
                    if (c == IAC) {
                        in = In::iac;
                    } else if (c == '\r' || c == '\n') {
                        if (c == '\n' && line.empty() && last_cr) {
                            last_cr = false;
                            break;  // CR LF
                        }
                        last_cr = c == '\r';
                        lines.push_back(line);
                        line.clear();
                    } else if (c == 0) {
                        // CR NUL
                    } else if (c == 0x7f || c == 0x08) {
                        if (!line.empty()) line.pop_back();
                    } else if (c == 0x03) {
                        interrupted = true;
                        line.clear();
                    } else if (c == 0x04) {
                        lines.push_back("exit");  // Ctrl-D
                    } else if (c >= 0x20 || c == '\t') {
                        last_cr = false;
                        line += static_cast<char>(c);
                    }
                    break;
                case In::iac:
                    if (c == IAC) {
                        line += static_cast<char>(c);
                        in = In::data;
                    } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                        in = In::option;
                    } else if (c == SB) {
                        in = In::sub;
                    } else {
                        if (c == IP) {
                            interrupted = true;
                            line.clear();
                        }
                        in = In::data;
                    }
                    break;
                case In::option:
                    in = In::data;  // we do not negotiate further
                    break;
                case In::sub:
                    if (c == IAC) in = In::sub_iac;
                    break;
                case In::sub_iac: in = c == SE ? In::data : In::sub; break;
            }
        }
        return lines;
    }
    bool last_cr = false;
};

Telnet::Telnet(Console& console, const Auth& auth, Options options)
    : console_(console), auth_(auth), options_(std::move(options)) {}

Telnet::~Telnet() { stop(); }

void Telnet::start() {
    std::string host = "0.0.0.0";
    std::string port = options_.listen;
    if (const auto colon = options_.listen.rfind(':'); colon != std::string::npos) {
        host = options_.listen.substr(0, colon);
        port = options_.listen.substr(colon + 1);
        if (host.empty()) host = "0.0.0.0";
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(std::stoi(port)));
    if (::inet_pton(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &addr.sin_addr) != 1) {
        throw std::runtime_error("telnet: invalid address '" + options_.listen + "' (use IPv4 HOST:PORT)");
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listen_fd_, 8) != 0) {
        const auto err = std::string(std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("telnet: cannot listen on " + options_.listen + ": " + err);
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    endpoint_ = host + ":" + std::to_string(ntohs(addr.sin_port));
    running_ = true;
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void Telnet::stop() {
    if (!running_.exchange(false)) return;
    if (accept_thread_.joinable()) accept_thread_.join();
    if (listen_fd_ >= 0) ::close(listen_fd_);
    listen_fd_ = -1;
    std::list<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard lock(mutex_);
        sessions.swap(sessions_);
    }
    for (auto& s : sessions) {
        s->print("\nserver shutting down\n");
        s->open = false;
        ::shutdown(s->fd, SHUT_RDWR);
        if (s->thread.joinable()) s->thread.join();
    }
}

std::size_t Telnet::sessions() const {
    std::lock_guard lock(mutex_);
    std::size_t n = 0;
    for (const auto& s : sessions_) n += s->open ? 1 : 0;
    return n;
}

void Telnet::accept_loop() {
    while (running_) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len, SOCK_CLOEXEC);
        if (fd < 0) continue;
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        // A client that stops reading is disconnected instead of blocking whoever
        // writes to it (the logging thread when it follows the log).
        const timeval send_timeout{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        auto s = std::make_shared<Session>();
        s->fd = fd;
        s->peer = std::format("{}:{}", ip, ntohs(peer.sin_port));

        std::lock_guard lock(mutex_);
        // Forget closed sessions.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (!(*it)->open) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
        if (sessions_.size() >= options_.max_sessions) {
            s->print(std::format("too many sessions ({}), try again later\n", options_.max_sessions));
            ::close(fd);
            continue;
        }
        sessions_.push_back(s);
        s->thread = std::thread([this, s] { serve(s); });
    }
}

void Telnet::serve(std::shared_ptr<Session> s) {
    log::get()->info("telnet: session from {}", s->peer);
    Console::Session cs;
    cs.channel = "telnet";
    cs.client = s->peer;
    cs.role = auth_.default_role();
    std::weak_ptr<Session> weak = s;
    cs.output = [weak](std::string_view text) {
        if (auto p = weak.lock()) p->print(text);
    };

    // Read one line (handles telnet negotiation); empty optional on close / idle timeout.
    std::deque<std::string> pending;
    auto read_line = [&]() -> std::optional<std::string> {
        while (s->open && running_) {
            if (!pending.empty()) {
                auto l = std::move(pending.front());
                pending.pop_front();
                return l;
            }
            pollfd p{s->fd, POLLIN, 0};
            const int r = ::poll(&p, 1, 200);
            if (r == 0) {
                if (options_.idle_s > 0 &&
                    std::chrono::steady_clock::now() - s->last_input > std::chrono::seconds(options_.idle_s)) {
                    s->print("\nidle timeout\n");
                    return std::nullopt;
                }
                continue;
            }
            char buf[512];
            const auto n = ::recv(s->fd, buf, sizeof(buf), 0);
            if (n <= 0) return std::nullopt;
            s->last_input = std::chrono::steady_clock::now();
            for (auto& l : s->feed(buf, static_cast<std::size_t>(n))) pending.push_back(std::move(l));
            if (s->interrupted) {
                s->interrupted = false;
                s->print("^C\n" + options_.prompt);
            }
        }
        return std::nullopt;
    };

    if (!options_.banner.empty()) s->print(options_.banner + "\n");
    bool ok = true;
    if (auth_.enabled()) {
        ok = false;
        for (int attempt = 0; attempt < 3 && s->open; ++attempt) {
            s->print("login: ");
            auto name = read_line();
            if (!name) break;
            // Ask the client not to echo the password.
            s->send(std::string{static_cast<char>(IAC), static_cast<char>(WILL), static_cast<char>(ECHO_OPT)});
            s->print("password: ");
            auto password = read_line();
            s->send(std::string{static_cast<char>(IAC), static_cast<char>(WONT), static_cast<char>(ECHO_OPT)});
            s->print("\n");
            if (!password) break;
            if (auto user = auth_.by_password(*name, *password)) {
                cs.user = user->name;
                cs.role = user->role;
                ok = true;
                break;
            }
            log::get()->warn("telnet: failed login for '{}' from {}", *name, s->peer);
            s->print("login incorrect\n");
        }
    }
    if (ok) {
        s->print(
            std::format("signed in as {} ({}) - 'help' for commands, 'logs on' to follow the log, 'exit' to "
                        "leave\n",
                        cs.user.empty() ? "anonymous" : cs.user, to_string(cs.role)));
        std::vector<std::string> history;
        while (s->open && running_) {
            s->print(options_.prompt);
            auto line = read_line();
            if (!line) break;
            const auto args = Console::split(*line);
            if (args.empty()) continue;
            history.push_back(*line);
            const auto& cmd = args[0];
            if (cmd == "exit" || cmd == "quit") break;
            if (cmd == "clear") {
                s->send("\033[2J\033[H");
                continue;
            }
            if (cmd == "history") {
                for (std::size_t i = 0; i < history.size(); ++i)
                    s->print(std::format("{:>4}  {}\n", i + 1, history[i]));
                continue;
            }
            if (cmd == "color") {
                if (args.size() > 1) s->color = args[1] == "on";
                s->print(std::format("colours {}\n", s->color ? "on" : "off"));
                continue;
            }
            if (cmd == "whoami") {
                s->print(std::format("{} ({}) from {}\n", cs.user.empty() ? "anonymous" : cs.user, to_string(cs.role),
                                     s->peer));
                continue;
            }
            if (cmd == "logs") {
                s->log_listener.reset();
                if (args.size() > 1 && args[1] == "on") {
                    const auto min = args.size() > 2 ? log::parse_level(args[2]) : log::Level::info;
                    s->log_listener = log::add_listener([weak, min](const log::Record& r) {
                        auto p = weak.lock();
                        if (!p) return;
                        const auto level = r.level;
                        if (level < min) return;
                        const auto t = static_cast<std::time_t>(r.time_unix_ns / 1000000000);
                        std::tm tm{};
                        ::localtime_r(&t, &tm);
                        const bool c = p->color;
                        p->print(std::format("{:02}:{:02}:{:02} [{}{}{}] [{}] {}\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                                             c ? log::level_color(level) : "", log::short_name(level),
                                             c ? "\033[0m" : "", r.logger, r.message));
                    });
                    s->print("following the log (logs off to stop)\n");
                } else {
                    s->print("log following off (logs on [level] to follow)\n");
                }
                continue;
            }
            console_.execute(*line, cs);
        }
    }
    s->log_listener.reset();
    s->open = false;
    ::shutdown(s->fd, SHUT_RDWR);
    ::close(s->fd);
    log::get()->info("telnet: session from {} closed", s->peer);
}

}  // namespace forge
