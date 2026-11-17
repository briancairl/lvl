/**
 * @copyright 2026-present Brian Cairl
 *
 * @file jobs.cpp
 */

// C++ Standard Library
#include <thread>

// LVL
#include "lvl/core/jobs.hpp"

namespace lvl::jobs
{

std::size_t hardware_concurrency()
{
  if constexpr (kHasThreads)
  {
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
  }
  return 1;
}

}  // namespace lvl::jobs
