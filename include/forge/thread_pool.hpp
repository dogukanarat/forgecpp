#pragma once
// Thread pool built on BS::thread_pool.
//
//   forge::ThreadPool pool(4);
//   auto fut = pool.submit_task([] { return heavy(); });
//   pool.detach_task([] { fire_and_forget(); });
//   pool.detach_loop(0, n, [&](std::size_t i) { process(i); });
//   pool.wait();

#include <BS_thread_pool.hpp>

namespace forge {

using ThreadPool = BS::thread_pool<BS::tp::none>;
using PriorityThreadPool = BS::thread_pool<BS::tp::priority>;

}  // namespace forge
