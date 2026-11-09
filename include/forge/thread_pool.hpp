#pragma once
// Thread pool built on BS::thread_pool.
//
//   forge::ThreadPool pool(4);
//   auto fut = pool.submit_task([] { return heavy(); });
//   pool.detach_task([] { fire_and_forget(); });
//   pool.detach_loop(0, n, [&](std::size_t i) { process(i); });
//   pool.wait();
//
// forge::ThreadPool is a class (not an alias), so that forge/application.hpp
// can name it without this header: BS_thread_pool.hpp is only parsed by the
// files that use the pool.

#include <BS_thread_pool.hpp>

namespace forge {

class ThreadPool : public BS::thread_pool<BS::tp::none> {
public:
    using BS::thread_pool<BS::tp::none>::thread_pool;
};

using PriorityThreadPool = BS::thread_pool<BS::tp::priority>;

}  // namespace forge
