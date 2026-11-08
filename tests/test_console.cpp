#include <forge/console.hpp>
#include <forge/log.hpp>
#include <forge/testing.hpp>

#include <algorithm>

using Args = forge::Console::Args;

TEST_CASE("console: split") {
    CHECK(forge::Console::split("") == Args{});
    CHECK(forge::Console::split("  set  a   b ") == Args{"set", "a", "b"});
    CHECK(forge::Console::split(R"(say "hello world" 'x y' z\ w)") == Args{"say", "hello world", "x y", "z w"});
    CHECK(forge::Console::split(R"(q "a\"b" "")") == Args{"q", "a\"b", ""});
}

TEST_CASE("console: commands and executor") {
    forge::Console::Options o;
    o.redirect_logs = false;
    o.color = false;
    forge::Console console(o);

    Args seen;
    console.add("greet", "greet <name>  say hello", [&](const Args& a) { seen = a; });
    console.add("fail", "fail  always throws", [](const Args&) { throw std::runtime_error("nope"); });

    CHECK(console.execute("greet bob alice"));
    CHECK(seen == Args{"bob", "alice"});
    CHECK_FALSE(console.execute("unknown"));
    CHECK(console.execute("fail"));  // exception is reported, not propagated
    CHECK(console.execute("   "));

    int via_executor = 0;
    console.set_executor([&](std::function<void()> fn) {
        ++via_executor;
        fn();
    });
    CHECK(console.execute("greet x"));
    CHECK(console.execute("help"));  // builtins bypass the executor
    CHECK(via_executor == 1);

    const auto cmds = console.commands();
    CHECK(std::find(cmds.begin(), cmds.end(), "greet") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "help") != cmds.end());
    CHECK(console.remove("greet"));
    CHECK_FALSE(console.execute("greet"));
}

// ---------------------------------------------------------------------------
// The interactive loop, driven through pipes. With LINENOISE_ASSUME_TTY linenoise
// edits as on a terminal (keys, completion, hints) without needing one.

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct Pipes {
    int in[2]{-1, -1};   // test -> console
    int out[2]{-1, -1};  // console -> test
    std::string seen;    // everything the console wrote so far
    Pipes() {
        REQUIRE(::pipe(in) == 0);
        REQUIRE(::pipe(out) == 0);
        ::fcntl(out[0], F_SETFL, O_NONBLOCK);
    }
    ~Pipes() {
        for (int fd : {in[0], in[1], out[0], out[1]}) {
            if (fd >= 0) ::close(fd);
        }
    }
    void type(std::string_view keys) { REQUIRE(::write(in[1], keys.data(), keys.size()) == static_cast<ssize_t>(keys.size())); }
    void close_input() {
        ::close(in[1]);
        in[1] = -1;
    }
    const std::string& output() {
        char buf[4096];
        ssize_t n;
        while ((n = ::read(out[0], buf, sizeof(buf))) > 0) seen.append(buf, static_cast<std::size_t>(n));
        return seen;
    }
    bool wait_for(std::string_view text, std::chrono::milliseconds timeout = 3s) {
        return forge::testing::wait_until([&] { return output().find(text) != std::string::npos; }, timeout);
    }
};

forge::Console::Options pipe_options(const Pipes& p) {
    forge::Console::Options o;
    o.redirect_logs = false;
    o.color = false;
    o.prompt = "test> ";
    o.input_fd = p.in[0];
    o.output_fd = p.out[1];
    return o;
}

struct AssumeTty {
    AssumeTty() {
        ::setenv("LINENOISE_ASSUME_TTY", "1", 1);
        ::setenv("LINENOISE_COLS", "80", 1);
    }
    ~AssumeTty() {
        ::unsetenv("LINENOISE_ASSUME_TTY");
        ::unsetenv("LINENOISE_COLS");
    }
};

}  // namespace

TEST_CASE("console: line editing - completion, hints, output while typing, Ctrl-C, Ctrl-D") {
    AssumeTty tty;
    Pipes p;
    forge::Console console(pipe_options(p));
    std::mutex m;
    Args seen;
    console.add("greet", "greet <name>  say hello", [&](const Args& a) {
        std::lock_guard lock(m);
        seen = a;
        console.print("hello {}", a.empty() ? "?" : a[0]);
    });
    std::atomic<int> exits{0};
    console.on_exit([&] { ++exits; });
    console.start();
    REQUIRE(p.wait_for("test> "));

    // TAB completes the command name (with a space); the hint shows its arguments
    p.type("gre\t");
    CHECK(p.wait_for("greet "));
    CHECK(p.wait_for("<name>"));
    p.type("bob\r");
    CHECK(p.wait_for("hello bob"));
    {
        std::lock_guard lock(m);
        CHECK(seen == Args{"bob"});
    }

    // Output from another thread while a line is half typed: printed, the line redrawn
    p.type("gre");
    REQUIRE(p.wait_for("gre"));
    const auto before = p.output().size();
    std::thread([&] { console.write("from another thread\n"); }).join();
    REQUIRE(p.wait_for("from another thread"));
    CHECK(forge::testing::wait_until([&] {
        const auto& out = p.output();
        const auto at = out.find("from another thread", before);
        return at != std::string::npos && out.find("test> gre", at) != std::string::npos;
    }));
    p.type("et alice\r");  // the half-typed line is still there
    CHECK(p.wait_for("hello alice"));

    // Ctrl-C drops the line; unknown command is reported; history lists what ran
    p.type("garbage\x03");
    p.type("nope\r");
    CHECK(p.wait_for("unknown command 'nope'"));
    p.type("history\r");
    CHECK(p.wait_for("   2  greet alice"));
    CHECK(p.wait_for("   3  nope"));
    CHECK(p.output().find("  garbage") == std::string::npos);  // dropped with Ctrl-C, never in the history

    // Ctrl-D on an empty line leaves the console
    p.type("\x04");
    CHECK(forge::testing::wait_until([&] { return exits.load() == 1 && !console.running(); }));
    console.stop();
}

TEST_CASE("console: stop() interrupts a console waiting for input") {
    AssumeTty tty;
    Pipes p;
    forge::Console console(pipe_options(p));
    std::atomic<int> exits{0};
    console.on_exit([&] { ++exits; });
    console.start();
    REQUIRE(p.wait_for("test> "));
    const auto t0 = std::chrono::steady_clock::now();
    console.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    CHECK_FALSE(console.running());
    CHECK(exits.load() == 0);  // stop() is not the user leaving
}

TEST_CASE("console: a pipe is read line by line, end of input leaves") {
    Pipes p;  // no LINENOISE_ASSUME_TTY: plain lines, no prompt, no editing
    forge::Console console(pipe_options(p));
    std::vector<std::string> ran;
    std::mutex m;
    console.add("say", "say <word>  echo", [&](const Args& a) {
        std::lock_guard lock(m);
        ran.push_back(a.empty() ? "" : a[0]);
    });
    std::atomic<int> exits{0};
    console.on_exit([&] { ++exits; });
    console.start();
    p.type("say one\nsay two\r\n\nsay three");  // CRLF, an empty line, no final newline
    p.close_input();
    CHECK(forge::testing::wait_until([&] { return exits.load() == 1; }));
    console.stop();
    std::lock_guard lock(m);
    CHECK(ran == std::vector<std::string>{"one", "two", "three"});
    CHECK(p.output().find("test> ") == std::string::npos);
}

TEST_CASE("console: log lines go through the console while it runs") {
    AssumeTty tty;
    Pipes p;
    forge::log::Options lo;  // as an application sets logging up (the default logger feeds the console sink)
    lo.name = "console-test";
    forge::log::init(lo);
    auto o = pipe_options(p);
    o.redirect_logs = true;
    forge::Console console(o);
    console.start();
    REQUIRE(p.wait_for("test> "));
    p.type("par");
    REQUIRE(p.wait_for("par"));
    FORGE_WARN("a log line while typing {}", 42);
    CHECK(p.wait_for("a log line while typing 42"));
    CHECK(p.wait_for("test> par"));
    console.stop();
    // after the console: logs no longer go to its output
    const auto size = p.output().size();
    FORGE_WARN("after the console");
    std::this_thread::sleep_for(100ms);
    CHECK(p.output().size() == size);
}
