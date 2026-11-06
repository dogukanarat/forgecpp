#include "forge/console.hpp"

#include "forge/log.hpp"

#include <replxx.hxx>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>

namespace forge {
namespace {

using replxx::Replxx;

constexpr const char* kReset = "\033[0m";

const char* level_color(spdlog::level::level_enum lvl) {
    switch (lvl) {
        case spdlog::level::trace: return "\033[37m";
        case spdlog::level::debug: return "\033[36m";
        case spdlog::level::info: return "\033[32m";
        case spdlog::level::warn: return "\033[33m\033[1m";
        case spdlog::level::err: return "\033[31m\033[1m";
        case spdlog::level::critical: return "\033[1m\033[41m";
        default: return "";
    }
}

// spdlog sink writing through replxx, which keeps the prompt intact.
class ReplxxSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    ReplxxSink(Replxx& rx, bool color) : rx_(rx), color_(color) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        formatter_->format(msg, buf);
        std::string out(buf.data(), buf.size());
        if (color_ && msg.color_range_end > msg.color_range_start) {
            out.insert(msg.color_range_end, kReset);
            out.insert(msg.color_range_start, level_color(msg.level));
        }
        rx_.write(out.data(), static_cast<int>(out.size()));
    }
    void flush_() override {}

private:
    Replxx& rx_;
    bool color_;
};

std::size_t codepoints(std::string_view s) {
    return static_cast<std::size_t>(
        std::count_if(s.begin(), s.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

struct Console::Impl {
    struct Command {
        std::string help;
        Handler handler;
        Completer completer;
        bool builtin = false;
    };

    explicit Impl(Options o) : options(std::move(o)) {}

    Options options;
    Replxx rx;
    mutable std::mutex mutex;
    std::map<std::string, Command> commands;
    Executor executor;
    std::function<void()> exit_callback;

    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> exit_requested{false};
    std::mutex thread_mutex;
    std::thread thread;

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

    void write(std::string_view text) { rx.write(text.data(), static_cast<int>(text.size())); }

    void setup_replxx() {
        rx.set_max_history_size(options.max_history);
        rx.set_unique_history(true);
        rx.set_word_break_characters(" \t");
        rx.set_no_color(!options.color);
        rx.set_completion_count_cutoff(128);
        rx.set_hint_delay(100);
        rx.set_max_hint_rows(3);

        rx.set_completion_callback([this](const std::string& input, int& context_len) {
            Replxx::completions_t out;
            auto args = Console::split(input);
            const bool new_word = input.empty() || input.back() == ' ' || input.back() == '\t';
            const std::string current = (!new_word && !args.empty()) ? args.back() : std::string();
            if (!new_word && !args.empty()) args.pop_back();
            context_len = static_cast<int>(codepoints(current));

            std::vector<std::string> candidates;
            if (args.empty()) {
                candidates = names_with_prefix(current);
            } else if (auto cmd = find(args.front()); cmd && cmd->completer) {
                Args so_far(args.begin() + 1, args.end());
                for (auto& c : cmd->completer(so_far)) {
                    if (starts_with(c, current)) candidates.push_back(std::move(c));
                }
            }
            for (auto& c : candidates) out.emplace_back(c);
            return out;
        });

        rx.set_hint_callback([this](const std::string& input, int& context_len, Replxx::Color& color) {
            Replxx::hints_t hints;
            if (input.empty() || input.find_first_of(" \t") != std::string::npos) return hints;
            context_len = static_cast<int>(codepoints(input));
            color = Replxx::Color::GRAY;
            for (auto& name : names_with_prefix(input)) {
                if (name != input) hints.emplace_back(name);
            }
            return hints;
        });

        rx.set_highlighter_callback([this](const std::string& input, Replxx::colors_t& colors) {
            const auto start = input.find_first_not_of(" \t");
            if (start == std::string::npos) return;
            auto end = input.find_first_of(" \t", start);
            if (end == std::string::npos) end = input.size();
            const std::string word = input.substr(start, end - start);
            const auto color = find(word) ? Replxx::Color::BRIGHTGREEN : Replxx::Color::BRIGHTRED;
            const auto from = codepoints(std::string_view(input).substr(0, start));
            const auto to = from + codepoints(word);
            for (std::size_t i = from; i < to && i < colors.size(); ++i) colors[i] = color;
        });

        if (!options.history_file.empty()) rx.history_load(options.history_file);
    }

    void loop() {
        spdlog::sink_ptr previous;
        if (options.redirect_logs) {
            previous = log::set_console_sink(std::make_shared<ReplxxSink>(rx, options.color));
        }
        if (!options.banner.empty()) write(options.banner + "\n");

        while (!stop_requested.load() && !exit_requested.load()) {
            errno = 0;
            const char* raw = rx.input(options.prompt);
            if (raw == nullptr) {
                if (errno == EAGAIN && !stop_requested.load()) continue;  // Ctrl-C clears the line
                if (!stop_requested.load()) exit_requested = true;        // Ctrl-D / EOF
                break;
            }
            if (stop_requested.load()) break;
            std::string line(raw);
            if (line.find_first_not_of(" \t") == std::string::npos) continue;
            rx.history_add(line);
            self->execute(line);
        }

        if (!options.history_file.empty()) rx.history_save(options.history_file);
        if (options.redirect_logs) log::set_console_sink(previous);
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

    Console* self = nullptr;
};

Console::Console() : Console(Options{}) {}

Console::Console(Options options) : impl_(std::make_shared<Impl>(std::move(options))) {
    impl_->self = this;
    impl_->setup_replxx();

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
            out += fmt::format("  {:<{}}  {}\n", name, width, summary);
        }
        write(out);
    });
    add("exit", "exit  leave the console", [this](const Args&) { impl_->exit_requested = true; });
    add("quit", "quit  leave the console", [this](const Args&) { impl_->exit_requested = true; });
    add("clear", "clear  clear the screen", [this](const Args&) { impl_->rx.clear_screen(); });
    add("history", "history  show command history", [this](const Args&) {
        auto scan = impl_->rx.history_scan();
        std::string out;
        for (int i = 1; scan.next(); ++i) out += fmt::format("{:>4}  {}\n", i, scan.get().text());
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

bool Console::execute(std::string_view line) {
    auto args = split(line);
    if (args.empty()) return true;
    const std::string name = args.front();
    args.erase(args.begin());

    auto cmd = impl_->find(name);
    if (!cmd) {
        error("unknown command '" + name + "' (type 'help')");
        return false;
    }
    auto invoke = [this, handler = cmd->handler, args = std::move(args)] {
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

void Console::write(std::string_view text) { impl_->write(text); }

void Console::error(std::string_view message) {
    if (impl_->options.color) {
        write(fmt::format("\033[31merror:\033[0m {}\n", message));
    } else {
        write(fmt::format("error: {}\n", message));
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
    if (impl_->running.load()) {
        // Wake replxx up: Ctrl-C makes input() return with EAGAIN.
        impl_->rx.emulate_key_press(Replxx::KEY::control('C'));
    }
    std::lock_guard lock(impl_->thread_mutex);
    if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()) {
        if (::isatty(STDIN_FILENO)) {
            impl_->thread.join();
        } else {
            // Piped stdin: a blocking read cannot be interrupted portably.
            impl_->thread.detach();
        }
    }
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
