// StateStore (persistent controls), Watchdog (systemd), HubRegistration
// (forge-hub) and the small helpers the services share.
#include "services.hpp"

#include "forge/control.hpp"
#include "forge/systemd.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <unistd.h>
#include <cstring>
#include <filesystem>

namespace forge::detail {

std::string host_name() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    return buf;
}

bool is_loopback(const std::string& host) { return host == "127.0.0.1" || host == "localhost" || host == "::1"; }

Caller console_caller() {
    Caller c;
    if (const auto* s = Console::current_session()) {
        c.channel = s->channel;
        c.client = s->client;
        c.user = s->user;
        c.role = s->role;
    } else {
        c.channel = "console";
    }
    return c;
}

// ---------------------------------------------------------------------------
// StateStore

void StateStore::restore(web::Dashboard& dashboard, const std::string& file, const Logger& logger) {
    file_ = file;
    if (file_.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) return;
    try {
        const auto saved = load_json(file_);
        std::size_t n = 0;
        for (const auto& id : dashboard.persistent_ids()) {
            if (!saved.contains(id)) continue;
            try {
                dashboard.set(id, saved[id]);
                ++n;
            } catch (const std::exception& e) {
                logger->warn("state: cannot restore '{}': {}", id, e.what());
            }
        }
        logger->info("state: restored {} control(s) from {}", n, file_);
    } catch (const std::exception& e) {
        logger->warn("state: ignoring {}: {}", file_, e.what());
    }
}

void StateStore::save(web::Dashboard& dashboard, const Logger& logger, bool force, ThreadPool* pool) {
    if (file_.empty() || !(dirty_.load() || force)) return;
    if (pool && writing_.load()) return;  // the last write is still on its way; dirty_ stays set
    dirty_ = false;
    json j = json::object();
    for (const auto& id : dashboard.persistent_ids()) j[id] = dashboard.get(id);
    if (!pool) {
        write(j, logger);
        return;
    }
    writing_ = true;
    pool->detach_task([this, j = std::move(j), logger] {
        write(j, logger);
        writing_ = false;
    });
}

void StateStore::write(const json& values, const Logger& logger) {
    try {
        save_json(file_, values);
    } catch (const std::exception& e) {
        logger->warn("state: cannot save {}: {}", file_, e.what());
    }
}

// ---------------------------------------------------------------------------
// Watchdog

bool Watchdog::tick(Health& health, double grace_s, const Logger& logger) {
    if (grace_s > 0) {
        if (auto failed = health.critical_failed_for(); failed && *failed > grace_s) {
            if (!paused_) {
                logger->critical(
                    "a critical component failed {:.0f} s ago: stopping watchdog pings (systemd restarts us)", *failed);
            }
            paused_ = true;
            return false;
        }
    }
    paused_ = false;
    return true;
}

// ---------------------------------------------------------------------------
// HubRegistration

void HubRegistration::start(const Config& cfg) {
    if (!cfg.value<bool>("hub.register")) return;
    const auto endpoint = cfg.value<std::string>("hub.endpoint");
    if (endpoint.empty()) return;
    const auto retry = std::chrono::seconds(std::max(cfg.value<int>("hub.retry_s"), 0));
    client_ = std::make_unique<hub::Client>(endpoint, retry, [this] {
        const auto c = app_.config();
        const auto& info = app_.info();
        forge_hub_app_registration r{};
        const auto put = [](auto& dst, std::string_view src) {
            const auto n = std::min(src.size(), sizeof(dst) - 1);
            std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        };
        put(r.name, app_.id());
        put(r.instance, app_.instance());
        put(r.version, info.version);
        put(r.description, info.description);
        r.pid = static_cast<std::uint32_t>(::getpid());
        auto* http = app_.http();
        r.http_port = http ? static_cast<std::uint32_t>(std::max(http->port(), 0)) : 0;
        for (const auto& [k, v] : app_.endpoints()) {
            if (k == "http" || r.endpoints_count == std::size(r.endpoints)) continue;
            auto& e = r.endpoints[r.endpoints_count++];
            put(e.name, k);
            put(e.url, v);
        }
        r.critical = c.value<bool>("hub.critical");
        put(r.group, c.value<std::string>("hub.group"));
        put(r.title, c.value<std::string>("hub.title"));
        put(r.unit, hub::unit_of(::getpid()));
        put(r.forgecpp, version_string);
        return r;
    });
    client_->start();
}

void HubRegistration::stop(forge_hub_unregister_reason reason, int exit_code) {
    if (client_) client_->stop(reason, exit_code);
}

}  // namespace forge::detail
