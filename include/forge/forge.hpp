#pragma once
// forgecpp umbrella header: everything in one include.
//
//   #include <forge/forge.hpp>

#include "forge/alarms.hpp"
#include "forge/application.hpp"
#include "forge/audit.hpp"
#include "forge/auth.hpp"
#include "forge/base64.hpp"
#include "forge/buffer_pool.hpp"
#include "forge/c2.hpp"
#include "forge/can.hpp"
#include "forge/channel.hpp"
#include "forge/component.hpp"
#include "forge/config.hpp"
#include "forge/config_schema.hpp"
#include "forge/console.hpp"
#include "forge/control.hpp"
#include "forge/error.hpp"
#include "forge/events.hpp"
#include "forge/gpio.hpp"
#include "forge/health.hpp"
#include "forge/http.hpp"
#include "forge/hub.hpp"
#include "forge/json.hpp"
#include "forge/log.hpp"
#include "forge/metrics.hpp"
#include "forge/pb.hpp"
#include "forge/remote.hpp"
#include "forge/rt.hpp"
#include "forge/scheduler.hpp"
#include "forge/serial.hpp"
#include "forge/status.hpp"
#include "forge/subscription.hpp"
#include "forge/systemd.hpp"
#include "forge/telnet.hpp"
#include "forge/thread_pool.hpp"
#include "forge/tmtc.hpp"
#include "forge/trace.hpp"
#include "forge/version.hpp"
#include "forge/web.hpp"
#include "forge/zmq.hpp"

#include <CLI/CLI.hpp>

namespace forge {
using namespace std::chrono_literals;
}  // namespace forge
