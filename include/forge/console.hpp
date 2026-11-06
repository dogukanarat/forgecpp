#pragma once
// Interactive command console (REPL) built on replxx: history, tab completion,
// hints and syntax colouring. While it runs, log output is routed through the
// console so log lines never corrupt the prompt.
//
//   forge::Console console({.prompt = "mydaemon> "});
//   console.add("greet", "greet <name>  say hello", [&](const forge::Console::Args& a) {
//       console.print("hello {}", a.empty() ? "world" : a[0]);
//   });
//   console.run();   // blocks until `exit`, Ctrl-D or stop()

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
    };

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
    /// or stop() is called.
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
