#include "forge/rt.hpp"

#include "forge/config.hpp"

#include <spdlog/fmt/fmt.h>

#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace forge::rt {
namespace {

int policy_of(const std::string& p) {
    if (p == "fifo") return SCHED_FIFO;
    if (p == "rr") return SCHED_RR;
    if (p == "batch") return SCHED_BATCH;
    if (p == "idle") return SCHED_IDLE;
    if (p == "other") return SCHED_OTHER;
    throw ConfigError("unknown thread policy '" + p + "' (other|fifo|rr|batch|idle)");
}

const char* policy_name(int p) {
    switch (p) {
        case SCHED_FIFO: return "fifo";
        case SCHED_RR: return "rr";
        case SCHED_BATCH: return "batch";
        case SCHED_IDLE: return "idle";
        default: return "other";
    }
}

}  // namespace

std::optional<ThreadSettings> thread_settings(const Config& cfg, const std::string& name) {
    const auto j = cfg.section("threads." + name).to_json();
    if (!j.is_object() || j.empty()) return std::nullopt;
    ThreadSettings s;
    s.policy = j.value("policy", std::string("other"));
    s.priority = j.value("priority", 0);
    if (j.contains("nice")) s.nice = j["nice"].get<int>();
    if (j.contains("cpus")) {
        for (const auto& c : j["cpus"]) s.cpus.push_back(c.get<int>());
    }
    policy_of(s.policy);  // validate
    return s;
}

std::string apply(const ThreadSettings& s) {
    std::string errors;
    auto add = [&](const std::string& e) { errors += (errors.empty() ? "" : "; ") + e; };
    if (!s.cpus.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int c : s.cpus) {
            if (c >= 0 && c < CPU_SETSIZE) CPU_SET(c, &set);
        }
        if (int r = pthread_setaffinity_np(pthread_self(), sizeof(set), &set); r != 0) {
            add(fmt::format("affinity: {}", std::strerror(r)));
        }
    }
    const int policy = policy_of(s.policy);
    sched_param sp{};
    sp.sched_priority = (policy == SCHED_FIFO || policy == SCHED_RR) ? s.priority : 0;
    if (int r = pthread_setschedparam(pthread_self(), policy, &sp); r != 0) {
        add(fmt::format("policy {} priority {}: {} (needs CAP_SYS_NICE or an rtprio limit)", s.policy, s.priority,
                        std::strerror(r)));
    }
    if (s.nice) {
        // nice is per thread on Linux (setpriority on the thread id).
        if (::setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), *s.nice) != 0) {
            add(fmt::format("nice {}: {}", *s.nice, std::strerror(errno)));
        }
    }
    return errors;
}

std::string lock_memory() {
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        return fmt::format("mlockall: {} (needs CAP_IPC_LOCK or a memlock limit; systemd: LimitMEMLOCK=infinity)",
                           std::strerror(errno));
    }
    return {};
}

std::string describe_current_thread() {
    int policy = 0;
    sched_param sp{};
    pthread_getschedparam(pthread_self(), &policy, &sp);
    cpu_set_t set;
    CPU_ZERO(&set);
    std::string cpus;
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0) {
        for (int c = 0; c < CPU_SETSIZE; ++c) {
            if (CPU_ISSET(c, &set)) cpus += (cpus.empty() ? "" : ",") + std::to_string(c);
        }
    }
    return fmt::format("policy {} priority {} cpus {}", policy_name(policy), sp.sched_priority, cpus);
}

}  // namespace forge::rt
