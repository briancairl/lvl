// C++ Standard Library
#include <atomic>
#include <cstdlib>
#include <new>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/json.hpp"
#include "lvl/core/memory.hpp"

namespace
{
std::atomic<std::size_t> heap_allocations{0};
}  // namespace

// Counts every global heap allocation made by this test binary
void* operator new(std::size_t n)
{
  ++heap_allocations;
  if (void* p = std::malloc(n == 0 ? 1 : n); p != nullptr)
  {
    return p;
  }
  std::abort();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

TEST(JsonMemory, ParseEditDumpMakesNoHeapAllocations)
{
  lvl::memory::Arena arena;
  lvl::memory::create(arena, 1 << 20, "json");
  lvl::json::install(arena);

  constexpr std::string_view kText = R"({
    "name": "a level with a fairly long name that defeats small-string optimization",
    "size": [128, 64],
    "layers": [
      {"name": "ground layer with a long name", "kind": "tiles", "data": [1, 2, 3, 4, 5, 6, 7, 8]},
      {"name": "height layer with a long name", "kind": "height", "data": [0.5, 0.25, 0.125]}
    ]
  })";

  // No gtest assertions inside the measured region (they may allocate themselves)
  bool parsed = false;
  std::size_t dumped_size = 0;
  const std::size_t before = heap_allocations.load();
  {
    lvl::json::Scope scope;
    lvl::Json j;
    parsed = lvl::json::parse(kText, j).ok;
    j["layers"][0]["name"] = "renamed ground layer with an equally long name";
    j["extra"] = lvl::Json::array({1, 2, 3});
    const lvl::JsonString text = lvl::json::dump(j, 2);
    dumped_size = text.size();
  }
  const std::size_t after = heap_allocations.load();
  EXPECT_TRUE(parsed);
  EXPECT_GT(dumped_size, kText.size() / 2);
  EXPECT_EQ(after, before);
  EXPECT_EQ(arena.utilized, 0UL);
  EXPECT_GT(arena.peak, 0UL);

  lvl::json::uninstall();
  lvl::memory::destroy(arena);
}
