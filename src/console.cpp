#include "forge/console.hpp"

#include "forge/log.hpp"

#include <linenoise.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace forge {
namespace {

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

void write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const auto n = ::write(fd, text.data(), text.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(n));
    }
}

/// In raw mode the terminal does not turn '\n' into "\r\n": do it here.
std::string crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\n') out += '\r';
        out += c;
    }
    return out;
}

// linenoise's callbacks are plain C functions without user data: they reach the
// console that is reading input through this (one interactive console at a time).
struct LineHooks {
    std::function<std::vector<std::string>(const std::string&)> complete;
    std::function<std::string(const std::string&)> hint;
    bool color = true;
};
std::atomic<LineHooks*> g_hooks{nullptr};

extern "C" void forge_console_complete(const char* buf, linenoiseCompletions* lc) {
    if (auto* h = g_hooks.load()) {
        for (const auto& c : h->complete(buf)) linenoiseAddCompletion(lc, c.c_str());
    }
}

extern "C" char* forge_console_hint(const char* buf, int* color, int* bold) {
    auto* h = g_hooks.load();
    if (!h) return nullptr;
    const std::string hint = h->hint(buf);
    if (hint.empty()) return nullptr;
    *color = h->color ? 90 : -1;  // grey
    *bold = 0;
    return ::strdup(hint.c_str());
}

extern "C" void forge_console_free_hint(void* hint) { std::free(hint); }

thread_local const Console::Session* tls_session = nullptr;

struct SessionScope {
    explicit SessionScope(const Console::Session* s) : previous(tls_session) { tls_session = s; }
    ~SessionScope() { tls_session = previous; }
    const Console::Session* previous;
};

Console::Session& local_session() {
    static Console::Session s;
    return s;
}

}  // namespace

const Console::Session* Console::current_session() { return tls_session; }

struct Console::Impl {
    struct Command {
        std::string help;
        Handler handler;
        Completer completer;
        bool builtin = false;
    };

    explicit Impl(Options o) : options(std::move(o)) {}

    Options options;
    mutable std::mutex mutex;  // commands, executor, exit callback
    std::map<std::string, Command> commands;
    Executor executor;
    std::function<void()> exit_callback;

    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> exit_requested{false};
    std::mutex thread_mutex;
    std::thread thread;

    // Output: while a line is being edited, text from any thread is queued and
    // printed by the console thread (line hidden, text, line shown again).
    std::mutex out_mutex;
    bool editing = false;
    std::deque<std::string> pending;
    int wake[2] = {-1, -1};  // pipe: output queued or stop requested

    linenoiseState line{};
    char line_buf[4096]{};
    std::deque<std::string> history;  // for the `history` command (linenoise keeps its own copy)
    LineHooks hooks;
    std::string plain_input;  // input that is not a terminal: bytes not yet split into lines

    Console* self = nullptr;

    std::optional<Command> find(const std::string& name) const {
        std::lock_guard lock(mutex);
        auto it = commands.find(name);
        if (it == commands.end()) return std::nullopt;
        return it->second;
    }

    std::vector<std::string> names_with_prefix(std::string_view prefix) const {
        std::vector<std::string> out;
        std::lock_guard lock(mutex);
        for (const auto& [name, cmd] : commands) {
            if (starts_with(name, prefix)) out.push_back(name);
        }
        return out;
    }

    // --- output -------------------------------------------------------------------
    void write(std::string_view text) {
        std::lock_guard lock(out_mutex);
        if (editing) {
            pending.emplace_back(text);
            notify();
            return;
        }
        write_all(options.output_fd, text);
    }

    void notify() {
        if (wake[1] >= 0) {
            const char c = 1;
            [[maybe_unused]] auto n = ::write(wake[1], &c, 1);  // non-blocking: a full pipe is already a wake-up
        }
    }

    /// Console thread, while editing: print what other threads queued.
    void print_pending() {
        std::lock_guard lock(out_mutex);
        if (pending.empty()) return;
        linenoiseHide(&line);
        for (const auto& text : pending) write_all(options.output_fd, crlf(text));
        pending.clear();
        linenoiseShow(&line);
    }

    // --- completion and hints -------------------------------------------------------
    std::vector<std::string> complete(const std::string& input) const {
        // linenoise replaces the whole line: candidates are full lines.
        const auto cut = input.find_last_of(" \t");
        const std::string base = cut == std::string::npos ? std::string() : input.substr(0, cut + 1);
        const std::string current = input.substr(base.size());
        auto args = Console::split(base);
        std::vector<std::string> out;
        if (args.empty()) {
            for (auto& name : names_with_prefix(current)) out.push_back(name + " ");
        } else if (auto cmd = find(args.front()); cmd && cmd->completer) {
            Args so_far(args.begin() + 1, args.end());
            for (auto& c : cmd->completer(so_far)) {
                if (starts_with(c, current)) out.push_back(base + c);
            }
        }
        return out;
    }

    std::string hint(const std::string& input) const {
        if (input.empty()) return {};
        const auto space = input.find_first_of(" \t");
        if (space == std::string::npos) {  // a command name being typed: the rest of the first match
            for (auto& name : names_with_prefix(input)) {
                if (name != input) return name.substr(input.size());
            }
            return {};
        }
        // "name " with nothing after it yet: its arguments from the help text ("name <args>  description")
        if (input.find_first_not_of(" \t", space) != std::string::npos) return {};
        const auto cmd = find(input.substr(0, space));
        if (!cmd || !starts_with(cmd->help, input.substr(0, space))) return {};
        const auto usage_end = cmd->help.find("  ");
        std::string usage = cmd->help.substr(space, usage_end == std::string::npos ? std::string::npos : usage_end - space);
        usage.erase(0, usage.find_first_not_of(' '));
        return usage;
    }

    // --- input ------------------------------------------------------------------------
    /// One line from a terminal, edited with linenoise. nullopt: stop or end of input.
    std::optional<std::string> read_edited() {
        {
            std::lock_guard lock(out_mutex);
            if (linenoiseEditStart(&line, options.input_fd, options.output_fd, line_buf, sizeof(line_buf),
                                   options.prompt.c_str()) == -1) {
                return read_plain();  // cannot use the terminal (e.g. it went away)
            }
            editing = true;
        }
        print_pending();
        auto finish = [this] {
            std::lock_guard lock(out_mutex);
            linenoiseEditStop(&line);
            editing = false;
            for (const auto& text : pending) write_all(options.output_fd, text);
            pending.clear();
        };
        for (;;) {
            pollfd fds[2] = {{options.input_fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                finish();
                return std::nullopt;
            }
            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (::read(wake[0], drain, sizeof(drain)) > 0) {
                }
                if (stop_requested.load()) {
                    finish();
                    return std::nullopt;
                }
                print_pending();
            }
            if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            errno = 0;
            char* result = linenoiseEditFeed(&line);
            if (result == linenoiseEditMore) continue;
            const int err = errno;
            finish();
            if (result) {
                std::string text(result);
                linenoiseFree(result);
                return text;
            }
            if (err == EAGAIN) return std::string();  // Ctrl-C: drop the line, new prompt
            if (!stop_requested.load()) exit_requested = true;  // Ctrl-D or end of input
            return std::nullopt;
        }
    }

    /// One line from a pipe or a file (no editing, no prompt); stop() interrupts it too.
    std::optional<std::string> read_plain() {
        for (;;) {
            if (auto nl = plain_input.find('\n'); nl != std::string::npos) {
                std::string text = plain_input.substr(0, nl);
                plain_input.erase(0, nl + 1);
                if (!text.empty() && text.back() == '\r') text.pop_back();
                return text;
            }
            pollfd fds[2] = {{options.input_fd, POLLIN, 0}, {wake[0], POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                if (errno == EINTR) continue;
                return std::nullopt;
            }
            if (fds[1].revents & POLLIN) {
                char drain[64];
                while (::read(wake[0], drain, sizeof(drain)) > 0) {
                }
                if (stop_requested.load()) return std::nullopt;
            }
            if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char chunk[1024];
            const auto n = ::read(options.input_fd, chunk, sizeof(chunk));
            if (n > 0) {
                plain_input.append(chunk, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            // End of input: a last line without '\n' still counts.
            if (!stop_requested.load()) exit_requested = true;
            if (plain_input.empty()) return std::nullopt;
            std::string text = std::move(plain_input);
            plain_input.clear();
            return text;
        }
    }

    void add_history(const std::string& text) {
        linenoiseHistoryAdd(text.c_str());
        add_history_local(text);
    }

    void load_history() {
        linenoiseHistorySetMaxLen(options.max_history);
        if (options.history_file.empty()) return;
        linenoiseHistoryLoad(options.history_file.c_str());
        std::ifstream in(options.history_file);
        for (std::string text; std::getline(in, text);) {
            if (!text.empty()) add_history_local(text);
        }
    }
    void add_history_local(const std::string& text) {
        if (history.empty() || history.back() != text) history.push_back(text);
        while (static_cast<int>(history.size()) > options.max_history) history.pop_front();
    }

    void loop() {
        {
            std::lock_guard lock(out_mutex);  // stop() may notify from another thread
            if (::pipe2(wake, O_NONBLOCK | O_CLOEXEC) != 0) wake[0] = wake[1] = -1;
        }
        hooks.complete = [this](const std::string& in) { return complete(in); };
        hooks.hint = [this](const std::string& in) { return hint(in); };
        hooks.color = options.color;
        g_hooks = &hooks;
        linenoiseSetCompletionCallback(forge_console_complete);
        linenoiseSetHintsCallback(forge_console_hint);
        linenoiseSetFreeHintsCallback(forge_console_free_hint);
        linenoiseSetMultiLine(1);
        load_history();
        // A terminal without a window size (serial consoles often have 0 columns)
        // makes linenoise ask the terminal for the cursor position and wait for an
        // answer that may never come: give it a width instead ($COLUMNS or 80).
        if (!std::getenv("LINENOISE_COLS")) {
            winsize ws{};
            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
                const char* columns = std::getenv("COLUMNS");
                ::setenv("LINENOISE_COLS", columns && std::atoi(columns) > 0 ? columns : "80", 0);
            }
        }

        log::TerminalWriter previous;
        if (options.redirect_logs) {
            // log lines are printed above the line being typed, coloured if the console is
            previous = log::set_terminal_writer([this](std::string_view plain, std::string_view coloured) {
                write(options.color ? coloured : plain);
            });
        }
        if (!options.banner.empty()) write(options.banner + "\n");

        const bool terminal = ::isatty(options.input_fd) || std::getenv("LINENOISE_ASSUME_TTY");
        while (!stop_requested.load() && !exit_requested.load() && wake[0] >= 0) {
            auto text = terminal ? read_edited() : read_plain();
            if (!text) break;
            if (text->find_first_not_of(" \t") == std::string::npos) continue;
            add_history(*text);
            self->execute(*text);
        }

        if (!options.history_file.empty()) linenoiseHistorySave(options.history_file.c_str());
        if (options.redirect_logs) log::set_terminal_writer(std::move(previous));
        g_hooks = nullptr;
        {
            std::lock_guard lock(out_mutex);
            for (int& fd : wake) {
                if (fd >= 0) ::close(fd);
                fd = -1;
            }
        }
        running = false;

        if (exit_requested.load() && !stop_requested.load()) {
            std::function<void()> cb;
            {
                std::lock_guard lock(mutex);
                cb = exit_callback;
            }
            if (cb) cb();
        }
    }
};

Console::Console() : Console(Options{}) {}

Console::Console(Options options) : impl_(std::make_shared<Impl>(std::move(options))) {
    impl_->self = this;

    add("help", "help [command]  list commands or show help for one", [this](const Args& args) {
        std::lock_guard lock(impl_->mutex);
        if (!args.empty()) {
            auto it = impl_->commands.find(args[0]);
            if (it == impl_->commands.end()) {
                error("unknown command '" + args[0] + "'");
            } else {
                write(it->second.help + "\n");
            }
            return;
        }
        std::size_t width = 0;
        for (const auto& [name, cmd] : impl_->commands) width = std::max(width, name.size());
        std::string out;
        for (const auto& [name, cmd] : impl_->commands) {
            std::string summary = cmd.help;
            // Drop the usage part ("name <args>  description") for the overview.
            if (auto sep = summary.find("  "); sep != std::string::npos && starts_with(summary, name)) {
                summary = summary.substr(summary.find_first_not_of(' ', sep));
            }
            out += std::format("  {:<{}}  {}\n", name, width, summary);
        }
        write(out);
    });
    add("exit", "exit  leave the console", [this](const Args&) { impl_->exit_requested = true; });
    add("quit", "quit  leave the console", [this](const Args&) { impl_->exit_requested = true; });
    add("clear", "clear  clear the screen", [this](const Args&) { write("\033[H\033[2J"); });
    add("history", "history  show command history", [this](const Args&) {
        std::string out;
        int i = 1;
        for (const auto& text : impl_->history) out += std::format("{:>4}  {}\n", i++, text);
        write(out);
    });
    std::lock_guard lock(impl_->mutex);
    for (auto& [name, cmd] : impl_->commands) cmd.builtin = true;
}

Console::~Console() { stop(); }

Console& Console::add(const std::string& name, const std::string& help, Handler handler,
                      Completer completer) {
    std::lock_guard lock(impl_->mutex);
    impl_->commands[name] = Impl::Command{help, std::move(handler), std::move(completer), false};
    return *this;
}

bool Console::remove(const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    return impl_->commands.erase(name) > 0;
}

std::vector<std::string> Console::commands() const { return impl_->names_with_prefix(""); }

bool Console::execute(std::string_view line) { return execute(line, local_session()); }

bool Console::execute(std::string_view line, const Session& session) {
    // Unknown-command errors also go to the session.
    SessionScope outer(&session);
    auto args = split(line);
    if (args.empty()) return true;
    const std::string name = args.front();
    args.erase(args.begin());

    auto cmd = impl_->find(name);
    if (!cmd) {
        error("unknown command '" + name + "' (type 'help')");
        return false;
    }
    auto invoke = [this, handler = cmd->handler, args = std::move(args), session] {
        SessionScope scope(&session);
        try {
            handler(args);
        } catch (const std::exception& e) {
            error(e.what());
        }
    };
    Executor executor;
    if (!cmd->builtin) {
        std::lock_guard lock(impl_->mutex);
        executor = impl_->executor;
    }
    try {
        if (executor) {
            executor(invoke);
        } else {
            invoke();
        }
    } catch (const std::exception& e) {
        error(e.what());
    }
    return true;
}

void Console::set_executor(Executor executor) {
    std::lock_guard lock(impl_->mutex);
    impl_->executor = std::move(executor);
}

void Console::on_exit(std::function<void()> callback) {
    std::lock_guard lock(impl_->mutex);
    impl_->exit_callback = std::move(callback);
}

void Console::write(std::string_view text) {
    if (tls_session && tls_session->output) {
        tls_session->output(text);
        return;
    }
    impl_->write(text);
}

void Console::error(std::string_view message) {
    if (impl_->options.color) {
        write(std::format("\033[31merror:\033[0m {}\n", message));
    } else {
        write(std::format("error: {}\n", message));
    }
}

void Console::run() {
    if (impl_->running.exchange(true)) return;
    impl_->stop_requested = false;
    impl_->exit_requested = false;
    impl_->loop();
}

void Console::start() {
    std::lock_guard lock(impl_->thread_mutex);
    if (impl_->running.exchange(true)) return;
    impl_->stop_requested = false;
    impl_->exit_requested = false;
    impl_->thread = std::thread([impl = impl_] { impl->loop(); });
}

void Console::stop() {
    impl_->stop_requested = true;
    {
        std::lock_guard lock(impl_->out_mutex);
        impl_->notify();  // wakes the loop, whether it waits for a terminal or a pipe
    }
    std::lock_guard lock(impl_->thread_mutex);
    if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()) impl_->thread.join();
}

bool Console::running() const { return impl_->running.load(); }

Console::Args Console::split(std::string_view line) {
    Args out;
    std::string current;
    bool in_word = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < line.size()) {
                current += line[++i];
            } else {
                current += c;
            }
        } else if (c == '\'' || c == '"') {
            quote = c;
            in_word = true;
        } else if (c == '\\' && i + 1 < line.size()) {
            current += line[++i];
            in_word = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_word) {
                out.push_back(std::move(current));
                current.clear();
                in_word = false;
            }
        } else {
            current += c;
            in_word = true;
        }
    }
    if (in_word) out.push_back(std::move(current));
    return out;
}

}  // namespace forge
