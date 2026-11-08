#pragma once
// Interactive command console (REPL) built on linenoise: history, tab completion
// and hints (the rest of a command name, then its arguments from the help text).
// While it runs, log output and output from other threads is routed through the
// console: the line being typed is hidden, the text printed, the line shown again.
//
//   forge::Console console({.prompt = "mydaemon> "});
//   console.add("greet", "greet <name>  say hello", [&](const forge::Console::Args& a) {
//       console.print("hello {}", a.empty() ? "world" : a[0]);
//   });
//   console.run();   // blocks until `exit`, Ctrl-D or stop()

#include "forge/auth.hpp"

#include <spdlog/fmt/fmt.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forge {

class Console {
public:
    using Args = std::vector<std::string>;
    using Handler = std::function<void(const Args& args)>;
    /// Returns completion candidates for the argument being typed.
    using Completer = std::function<std::vector<std::string>(const Args& args_so_far)>;
    /// Runs a command; the default executes it inline on the console thread.
    using Executor = std::function<void(std::function<void()>)>;

    struct Options {
        std::string prompt = "> ";
        std::string history_file;      ///< empty = no persistent history
        int max_history = 1000;
        bool redirect_logs = true;     ///< route log output through the console
        bool color = true;
        std::string banner;            ///< printed when the console starts
        int input_fd = 0;              ///< terminal (or pipe) to read; default stdin
        int output_fd = 1;             ///< where the prompt and output go; default stdout
    };

    /// Who runs a command and where its output goes. The local console is
    /// admin; telnet sessions carry the logged-in user (see forge::Telnet).
    struct Session {
        std::string channel = "console";
        std::string client;
        std::string user;
        Role role = Role::admin;
        std::function<void(std::string_view)> output;  ///< empty: the local terminal
    };
    /// Session of the command running on this thread (nullptr outside commands).
    static const Session* current_session();

    Console();
    explicit Console(Options options);
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    /// Register a command. `help` is shown by the builtin `help` command.
    Console& add(const std::string& name, const std::string& help, Handler handler,
                 Completer completer = {});
    bool remove(const std::string& name);
    std::vector<std::string> commands() const;

    /// Execute a command line as if it was typed. Returns false for unknown commands.
    bool execute(std::string_view line);
    /// Execute for a remote session: output goes to session.output.
    bool execute(std::string_view line, const Session& session);

    /// Run commands through `executor` (e.g. on the application main loop).
    void set_executor(Executor executor);
    /// Called when the user leaves the console (exit, quit, Ctrl-D).
    void on_exit(std::function<void()> callback);

    /// Thread-safe output that does not disturb the prompt.
    void write(std::string_view text);
    template <class... T>
    void print(fmt::format_string<T...> format, T&&... args) {
        write(fmt::format(format, std::forward<T>(args)...) + "\n");
    }
    void error(std::string_view message);

    /// Read-eval-print loop on the calling thread. Returns when the user exits
    /// or stop() is called. A terminal gets line editing; a pipe or file is
    /// read line by line (no prompt). One interactive console at a time.
    void run();
    /// Run the loop on a background thread.
    void start();
    /// Thread-safe: interrupt the loop and wait for the background thread.
    void stop();
    bool running() const;

    /// Split a command line into words, honouring '…', "…" and backslash escapes.
    static Args split(std::string_view line);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace forge
