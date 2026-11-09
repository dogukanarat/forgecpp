// Application: metrics, the flight recorder, crash reports and the process
// figures the framework keeps up to date by itself.
#include "forge/application.hpp"

#include "services.hpp"

#include "forge/channel.hpp"
#include "forge/crash.hpp"
#include "forge/rt.hpp"
#include "forge/thread_pool.hpp"
#include "forge/trace.hpp"

#include <sys/resource.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>

namespace forge {

void Application::start_observability(const Config& cfg) {
    trace::configure(cfg.value<bool>("trace.enabled"), cfg.value<std::size_t>("trace.capacity"));
    audit_.configure(cfg.value<std::size_t>("audit.history"), cfg.value<std::string>("audit.file"));

    if (cfg.value<bool>("crash.enabled")) {
        std::string dir = cfg.value<std::string>("crash.dir");
        if (dir.empty()) {
            const char* state = std::getenv("STATE_DIRECTORY");  // systemd StateDirectory=
            dir = state ? std::string(state).substr(0, std::string(state).find(':')) : "/tmp";
        }
        if (auto report = crash::previous_report(info_.name, dir)) {
            std::ifstream in(*report);
            std::string first;
            std::getline(in, first);
            const auto kept = std::format(
                "{}/{}-crash-{}.txt", dir, info_.name,
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
            std::error_code ec;
            std::filesystem::rename(*report, kept, ec);
            logger_->error("the previous run crashed ({}); report: {}", first, ec ? *report : kept);
            alarms_.raise("forge.crashed", Severity::error,
                          std::format("The previous run crashed ({}). Report: {}", first, ec ? *report : kept));
        }
        crash::install(info_.name, dir);
    }
    if (cfg.value<bool>("process.mlockall")) {
        if (auto err = rt::lock_memory(); err.empty()) {
            logger_->info("memory locked (mlockall)");
        } else {
            logger_->warn("{}", err);
        }
    }

    // Process metrics, computed when read.
    auto& m = metrics::registry();
    m.gauge_fn("process.uptime_seconds", [this] { return std::chrono::duration<double>(uptime()).count(); }, "Uptime",
               "s");
    m.gauge_fn("forge.loop.queued", [this] { return static_cast<double>(tasks_->size_approx()); },
               "Tasks waiting for the main loop ([forge].max_pending_tasks bounds it)");
    m.gauge_fn("process.rss_bytes", [] {
        long pages = 0, rss = 0;
        if (FILE* f = std::fopen("/proc/self/statm", "r")) {
            if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
            std::fclose(f);
        }
        return static_cast<double>(rss) * static_cast<double>(::sysconf(_SC_PAGESIZE));
    }, "Resident memory", "bytes");
    m.gauge_fn("process.cpu_seconds", [] {
        rusage u{};
        ::getrusage(RUSAGE_SELF, &u);
        return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
               static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
    }, "CPU time (user + system)", "s");
    m.gauge_fn("process.threads", [] {
        std::error_code ec;
        std::size_t n = 0;
        for (auto it = std::filesystem::directory_iterator("/proc/self/task", ec);
             !ec && it != std::filesystem::directory_iterator(); ++it) {
            ++n;
        }
        return static_cast<double>(n);
    }, "Threads");
    m.gauge_fn("forge.pool.queued", [this] { return pool_ ? static_cast<double>(pool_->get_tasks_queued()) : 0.0; },
               "Tasks waiting for a worker thread");
    m.gauge_fn("forge.scheduler.timers",
               [this] { return scheduler_ ? static_cast<double>(scheduler_->pending()) : 0.0; }, "Pending timers");
    m.gauge_fn("forge.alarms.active", [this] { return static_cast<double>(alarms_.active_count()); }, "Active alarms");
    m.gauge_fn("forge.health.state", [this] { return static_cast<double>(health_.overall()); },
               "Overall health: 1 ok, 2 degraded, 3 failed");
}

}  // namespace forge
