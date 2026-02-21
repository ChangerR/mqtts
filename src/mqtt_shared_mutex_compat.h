#pragma once

#include <mutex>

#if __cplusplus >= 201703L
#include <shared_mutex>
#endif

namespace mqtt {
namespace compat {

#if __cplusplus >= 201703L
using shared_mutex = std::shared_mutex;
template <typename Mutex>
using shared_lock = std::shared_lock<Mutex>;
#else
using shared_mutex = std::mutex;
template <typename Mutex>
using shared_lock = std::unique_lock<Mutex>;
#endif

} // namespace compat
} // namespace mqtt

