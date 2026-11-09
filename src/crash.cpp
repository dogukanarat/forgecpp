#include "forge/crash.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <algorithm>

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define FORGE_HAVE_BACKTRACE 1
#endif

namespace forge::crash {
namespace {

// Everything the handler touches is preallocated: no malloc in signal context.
char g_path[512];
constexpr std::size_t kLogBytes = 16384;
char g_log[kLogBytes];
std::atomic<std::size_t> g_log_pos{0};
std::atomic<bool> g_installed{false};

void write_str(int fd, const char* s) { (void)!::write(fd, s, std::strlen(s)); }

void write_num(int fd, long v) {
    char buf[24];
    int n = 0;
    bool neg = v < 0;
    unsigned long u = neg ? static_cast<unsigned long>(-v) : static_cast<unsigned long>(v);
    do {
        buf[n++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u && n < 22);
    if (neg) buf[n++] = '-';
    char out[24];
    for (int i = 0; i < n; ++i) out[i] = buf[n - 1 - i];
    (void)!::write(fd, out, static_cast<std::size_t>(n));
}

const char* sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGFPE: return "SIGFPE";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        default: return "?";
    }
}

void handler(int sig, siginfo_t* info, void*) {
    const int fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write_str(fd, "signal ");
        write_num(fd, sig);
        write_str(fd, " (");
        write_str(fd, sig_name(sig));
        write_str(fd, ")\naddress 0x");
        char hex[20];
        const auto addr = reinterpret_cast<unsigned long>(info ? info->si_addr : nullptr);
        std::snprintf(hex, sizeof(hex), "%lx", addr);  // snprintf of an integer does not allocate in glibc
        write_str(fd, hex);
        write_str(fd, "\ntime ");
        write_num(fd, static_cast<long>(std::time(nullptr)));
        write_str(fd, "\npid ");
        write_num(fd, static_cast<long>(::getpid()));
        write_str(fd, "\n\nbacktrace:\n");
#ifdef FORGE_HAVE_BACKTRACE
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        ::backtrace_symbols_fd(frames, n, fd);
#else
        write_str(fd, "(not available on this C library)\n");
#endif
        write_str(fd, "\nlast log lines:\n");
        const std::size_t pos = g_log_pos.load();
        if (pos > kLogBytes) {
            const std::size_t start = pos % kLogBytes;
            (void)!::write(fd, g_log + start, kLogBytes - start);
            (void)!::write(fd, g_log, start);
        } else {
            (void)!::write(fd, g_log, pos);
        }
        ::close(fd);
    }
    // Default action: core dump / termination as without the handler.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

}  // namespace

void install(const std::string& app, const std::string& dir) {
    std::snprintf(g_path, sizeof(g_path), "%s/%s-crash.txt", dir.c_str(), app.c_str());
#ifdef FORGE_HAVE_BACKTRACE
    void* warm[1];
    ::backtrace(warm, 1);  // loads libgcc now, not in the signal handler
#endif
    struct sigaction sa {};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) ::sigaction(sig, &sa, nullptr);
    g_installed = true;
}

std::optional<std::string> previous_report(const std::string& app, const std::string& dir) {
    const auto p = std::filesystem::path(dir) / (app + "-crash.txt");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return std::nullopt;
    return p.string();
}

void remember_log_line(const char* data, std::size_t size) {
    if (!g_installed.load(std::memory_order_relaxed)) return;
    size = std::min(size, kLogBytes / 4);
    const std::size_t pos = g_log_pos.fetch_add(size);
    for (std::size_t i = 0; i < size; ++i) g_log[(pos + i) % kLogBytes] = data[i];
}

}  // namespace forge::crash
