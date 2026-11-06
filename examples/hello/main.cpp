// The smallest useful forgecpp daemon (lambda style).
//
//   ./hello                         # logs a greeting every second, Ctrl-C to stop
//   ./hello -D app.name=Yocto -v    # config override + debug logging
//   ./hello -i                      # with the interactive console
//   ./hello --http 8080             # curl localhost:8080/health

#include <forge/forge.hpp>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    forge::Application app({"hello", "1.0.0", "forgecpp hello world daemon"});

    int count = 0;
    app.cli().add_option("-n,--count", count, "Stop after N greetings (0 = forever)");

    app.hook_start([&](forge::Application& a) {
        const auto name = a.config().get_or<std::string>("app.name", "world");
        a.every(1s, [&a, &count, name, i = 0]() mutable {
            a.logger()->info("hello {} #{}", name, ++i);
            if (count > 0 && i >= count) a.stop();
        });
    });

    app.hook_console([](forge::Application& a, forge::Console& console) {
        console.add("uptime", "uptime  seconds since start", [&a, &console](const forge::Console::Args&) {
            console.print("{} s", std::chrono::duration_cast<std::chrono::seconds>(a.uptime()).count());
        });
    });

    return app.run(argc, argv);
}
