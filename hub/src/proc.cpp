#include "proc.hpp"

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
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

std::vector<std::string> split_nul(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < s.size()) {
        auto end = s.find('\0', start);
        if (end == std::string::npos) end = s.size();
        out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
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

std::string read_link(const std::string& path) {
    char buf[4096];
    const auto n = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return {};
    buf[n] = '\0';
    return buf;
}

}  // namespace

std::optional<LaunchInfo> read_launch(int pid, std::string* error) {
    LaunchInfo info;
    info.exe = read_link(proc_path(pid, "exe"));
    // An upgraded binary shows as "/usr/bin/x (deleted)": start the new one.
    static const std::string deleted = " (deleted)";
    if (info.exe.size() > deleted.size() &&
        info.exe.compare(info.exe.size() - deleted.size(), deleted.size(), deleted) == 0) {
        info.exe.erase(info.exe.size() - deleted.size());
    }
    info.cwd = read_link(proc_path(pid, "cwd"));
    const auto cmdline = read_file(proc_path(pid, "cmdline"));
    const auto environ = read_file(proc_path(pid, "environ"));
    struct stat st {};
    const bool exists = ::stat(proc_path(pid, "").c_str(), &st) == 0;
    if (!exists || !cmdline || cmdline->empty()) {
        if (error) *error = "process " + std::to_string(pid) + " does not exist";
        return std::nullopt;
    }
    if (info.exe.empty() || !environ) {
        if (error) {
            *error = "cannot read /proc/" + std::to_string(pid) +
                     " (run the hub as root or as the application's user to let it restart the application)";
        }
        return std::nullopt;
    }
    info.argv = split_nul(*cmdline);
    info.env = split_nul(*environ);
    info.uid = st.st_uid;
    info.gid = st.st_gid;
    if (info.cwd.empty()) info.cwd = "/";
    return info;
}

std::uint64_t start_ticks(int pid) {
    const auto f = stat_fields(pid);
    // field 22 (starttime) -> index 19 after the state at index 0 (field 3)
    if (f.size() < 20) return 0;
    try {
        return std::stoull(f[19]);
    } catch (...) {
        return 0;
    }
}

bool alive(int pid, std::uint64_t ticks) {
    if (pid <= 0) return false;
    const auto f = stat_fields(pid);
    if (f.size() < 20) return false;
    if (f[0] == "Z" || f[0] == "X") return false;  // zombie / dead
    if (ticks == 0) return true;
    try {
        return std::stoull(f[19]) == ticks;
    } catch (...) {
        return false;
    }
}

std::optional<Usage> usage(int pid) {
    const auto f = stat_fields(pid);
    if (f.size() < 22) return std::nullopt;
    Usage u;
    try {
        u.cpu_ticks = std::stoull(f[11]) + std::stoull(f[12]);  // utime (14), stime (15)
        const auto statm = read_file(proc_path(pid, "statm"));
        if (statm) {
            std::istringstream in(*statm);
            std::uint64_t size = 0, resident = 0;
            in >> size >> resident;
            u.rss_bytes = resident * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        }
    } catch (...) {
        return std::nullopt;
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

int spawn(const LaunchInfo& info, const std::string& log_file, std::string* error) {
    if (info.exe.empty() || info.argv.empty()) {
        if (error) *error = "nothing to start";
        return -1;
    }
    // Everything the child needs is prepared before fork(): after it, in a
    // multi-threaded process, only async-signal-safe calls are allowed.
    std::vector<char*> argv, envp;
    for (const auto& a : info.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (const auto& e : info.env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    const std::string out = log_file.empty() ? std::string("/dev/null") : log_file;
    const bool drop = ::geteuid() == 0 && (info.uid != 0 || info.gid != 0);

    int err_pipe[2];
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
        if (error) *error = std::string("pipe: ") + std::strerror(errno);
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        if (error) *error = std::string("fork: ") + std::strerror(errno);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        auto fail = [&](int code) {
            const int e = errno;
            const int msg[2] = {code, e};
            (void)!::write(err_pipe[1], msg, sizeof(msg));
            ::_exit(127);
        };
        ::setsid();  // own session: not killed with the hub
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);  // the hub blocks signals for its signal thread
        for (int s = 1; s < NSIG; ++s) ::signal(s, SIG_DFL);
        const int in = ::open("/dev/null", O_RDONLY);
        const int o = ::open(out.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0640);
        if (in < 0 || o < 0) fail(1);
        ::dup2(in, 0);
        ::dup2(o, 1);
        ::dup2(o, 2);
        if (::chdir(info.cwd.c_str()) != 0) fail(2);
        if (drop) {
            if (::setgroups(0, nullptr) != 0 || ::setgid(info.gid) != 0 || ::setuid(info.uid) != 0) fail(3);
        }
        ::execve(info.exe.c_str(), argv.data(), envp.data());
        fail(4);
    }
    ::close(err_pipe[1]);
    int msg[2] = {0, 0};
    const auto n = ::read(err_pipe[0], msg, sizeof(msg));  // EOF on a successful exec (O_CLOEXEC)
    ::close(err_pipe[0]);
    if (n == static_cast<ssize_t>(sizeof(msg))) {
        ::waitpid(pid, nullptr, 0);
        static const char* step[] = {"", "open output", "chdir " , "switch user", "execute"};
        if (error) {
            *error = std::string(step[msg[0] >= 1 && msg[0] <= 4 ? msg[0] : 4]) +
                     (msg[0] == 2 ? info.cwd : msg[0] == 4 ? " " + info.exe : "") + ": " + std::strerror(msg[1]);
        }
        return -1;
    }
    return pid;
}

bool reap(int pid, int* status) {
    if (pid <= 0) return false;
    int st = 0;
    const auto r = ::waitpid(pid, &st, WNOHANG);
    if (r == pid) {
        if (status) *status = st;
        return true;
    }
    return false;
}

CommandResult run_command(const std::vector<std::string>& args, std::chrono::milliseconds timeout) {
    CommandResult result;
    if (args.empty()) return result;
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
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
        ::execvp(argv[0], argv.data());
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
