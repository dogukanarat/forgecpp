#pragma once
// Logging from real-time threads.
//
// FORGE_RT_* hand the arguments to the logging background thread, which
// formats them: no formatting and, for numbers and strings, no allocation on
// the calling thread. The queue of each thread grows up to a limit and then
// drops messages instead of blocking (the background thread reports how many).
//
//   #include <forge/log_rt.hpp>
//   FORGE_RT_WARN(logger(), "late by {} us", late_us);   // logger(): a forge::Logger
//
// The format string must be a literal ({fmt} syntax, as std::format). Types
// other than numbers, strings, std::string_view and the std containers of
// <quill/std/...> need a Quill codec; format those with logger()->warn(...)
// instead. Messages logged this way do not carry the trace context.

#include "forge/log.hpp"

#include <quill/LogMacros.h>
#include <quill/Logger.h>
#include <quill/core/FrontendOptions.h>

namespace forge::log::detail {

/// Queues of the threads that log: start small, grow to 16 MiB, then drop (never block).
struct FrontendOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedDropping;
    static constexpr std::size_t initial_queue_capacity = 64u * 1024u;
    static constexpr std::uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr std::size_t unbounded_queue_max_capacity = 16u * 1024u * 1024u;
    static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};

using QuillLogger = quill::LoggerImpl<FrontendOptions>;

/// The Quill logger behind a forge::Logger.
QuillLogger* quill_logger(const Logger& logger);

}  // namespace forge::log::detail

#define FORGE_RT_TRACE(logger, fmt, ...) \
    QUILL_LOG_TRACE_L1(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
#define FORGE_RT_DEBUG(logger, fmt, ...) QUILL_LOG_DEBUG(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
#define FORGE_RT_INFO(logger, fmt, ...) QUILL_LOG_INFO(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
#define FORGE_RT_WARN(logger, fmt, ...) \
    QUILL_LOG_WARNING(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
#define FORGE_RT_ERROR(logger, fmt, ...) QUILL_LOG_ERROR(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
#define FORGE_RT_CRITICAL(logger, fmt, ...) \
    QUILL_LOG_CRITICAL(::forge::log::detail::quill_logger(logger), fmt, ##__VA_ARGS__)
