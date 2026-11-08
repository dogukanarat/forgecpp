#include "proc.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <thread>

namespace forgehub {
namespace {

std::string proc_path(int pid, const char* what) { return "/proc/" + std::to_string(pid) + "/" + what; }

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return ss.str();
}


/// Fields of /proc/<pid>/stat after the command name (field 3 = state is [0]).
std::vector<std::string> stat_fields(int pid) {
    const auto s = read_file(proc_path(pid, "stat"));
    if (!s) return {};
    const auto close = s->rfind(')');  // the command name may contain spaces and ')'
    if (close == std::string::npos) return {};
    std::istringstream in(s->substr(close + 2));
    std::vector<std::string> f;
    for (std::string x; in >> x;) f.push_back(x);
    return f;
}

// A decimal number; 0 when the text is not one.
std::uint64_t number(const std::string& s) {
    std::uint64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc() && end == s.data() + s.size() ? v : 0;
}


}  // namespace

std::uint64_t start_ticks(int pid) {
    const auto f = stat_fields(pid);
    // field 22 (starttime) -> index 19 after the state at index 0 (field 3)
    return f.size() < 20 ? 0 : number(f[19]);
}

bool alive(int pid, std::uint64_t ticks) {
    if (pid <= 0) return false;
    const auto f = stat_fields(pid);
    if (f.size() < 20) return false;
    if (f[0] == "Z" || f[0] == "X") return false;  // zombie / dead
    return ticks == 0 || number(f[19]) == ticks;
}

std::optional<Usage> usage(int pid) {
    const auto f = stat_fields(pid);
    if (f.size() < 22) return std::nullopt;
    Usage u;
    u.cpu_ticks = number(f[11]) + number(f[12]);  // utime (14), stime (15)
    if (const auto statm = read_file(proc_path(pid, "statm"))) {
        std::istringstream in(*statm);
        std::uint64_t size = 0, resident = 0;
        in >> size >> resident;
        u.rss_bytes = resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
    }
    return u;
}

long ticks_per_second() {
    static const long t = ::sysconf(_SC_CLK_TCK);
    return t > 0 ? t : 100;
}

std::string boot_id(const std::string& file) {
    auto s = read_file(file).value_or("");
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

CommandResult run_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    CommandResult result;
    if (args.empty()) return result;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // The hub's own systemd notification socket and watchdog are not the
    // child's: systemd rejects (and logs) messages from other processes.
    std::vector<std::string> env_strings;
    for (char** e = environ; *e; ++e) {
        const std::string_view v(*e);
        if (v.rfind("NOTIFY_SOCKET=", 0) == 0 || v.rfind("WATCHDOG_", 0) == 0) continue;
        env_strings.emplace_back(v);
    }
    std::vector<char*> envp;
    for (auto& v : env_strings) envp.push_back(v.data());
    envp.push_back(nullptr);
    int out[2];
    if (::pipe2(out, O_CLOEXEC) != 0) return result;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(out[0]);
        ::close(out[1]);
        return result;
    }
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::dup2(out[1], 1);
        ::dup2(out[1], 2);
        ::execvpe(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(out[1]);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[1024];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd p{out[0], POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
        const auto n = ::read(out[0], buf, sizeof(buf));
        if (n <= 0) break;
        if (result.output.size() < 64 * 1024) result.output.append(buf, static_cast<std::size_t>(n));
    }
    ::close(out[0]);
    int st = 0;
    while (::waitpid(pid, &st, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
            return result;  // timed out: exit_code -1
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    result.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == ' ')) {
        result.output.pop_back();
    }
    return result;
}

bool valid_unit_name(const std::string& unit) {
    if (unit.size() < 9 || unit.size() > 256 || unit[0] == '-') return false;
    if (unit.compare(unit.size() - 8, 8, ".service") != 0) return false;
    for (const char c : unit) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '@' ||
              c == ':' || c == '\\')) {
            return false;
        }
    }
    return true;
}

}  // namespace forgehub
