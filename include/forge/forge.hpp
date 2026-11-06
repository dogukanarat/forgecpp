#pragma once
// forgecpp umbrella header: everything in one include.
//
//   #include <forge/forge.hpp>

#include "forge/alarms.hpp"
#include "forge/application.hpp"
#include "forge/channel.hpp"
#include "forge/config.hpp"
#include "forge/console.hpp"
#include "forge/events.hpp"
#include "forge/http.hpp"
#include "forge/json.hpp"
#include "forge/log.hpp"
#include "forge/scheduler.hpp"
#include "forge/systemd.hpp"
#include "forge/thread_pool.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"

#include <CLI/CLI.hpp>

namespace forge {
using namespace std::chrono_literals;
}  // namespace forge
