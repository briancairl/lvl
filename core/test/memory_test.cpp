// C++ Standard Library
#include <cstdint>
#include <numeric>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/memory.hpp"

using namespace lvl;

TEST(Arena, CreateAllocateDestroy)
{
  memory::Arena arena;
  memory::create(arena, 1024, "test");
  ASSERT_TRUE(memory::is_valid(arena));
  auto* a = memory::allocate<std::uint8_t>(arena, 3);
  auto* b = memory::allocate<std::uint64_t>(arena, 2);
  EXPECT_NE(a, nullptr);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b) % alignof(std::uint64_t), 0UL);
  EXPECT_GE(arena.utilized, 3UL + 16UL);
  memory::destroy(arena);
  EXPECT_FALSE(memory::is_valid(arena));
}

TEST(Arena, AllocateWithInit)
{
  memory::Arena arena;
  memory::create(arena, 1024);
  const int* values = memory::allocate<int>(arena, 8, 5);
  EXPECT_EQ(std::accumulate(values, values + 8, 0), 40);
  memory::destroy(arena);
}

TEST(Arena, ExactCapacityFits)
{
  memory::Arena arena;
  memory::create(arena, 64);
  memory::allocate<std::uint8_t>(arena, 64);
  EXPECT_EQ(memory::available(arena), 0UL);
  memory::destroy(arena);
}

TEST(Arena, SubArenasPushPop)
{
  memory::Arena root;
  memory::create(root, 4096, "root");
  memory::Arena a;
  memory::Arena b;
  memory::push(root, a, 1024, "a");
  memory::push(root, b, 1024, "b");
  memory::allocate<char>(b, 1000);
  memory::pop(root, b);
  memory::pop(root, a);
  EXPECT_EQ(root.utilized, 0UL);
  memory::destroy(root);
}

TEST(Arena, ScopeRewinds)
{
  memory::Arena arena;
  memory::create(arena, 1024);
  memory::allocate<char>(arena, 10);
  const auto before = arena.utilized;
  {
    memory::Scope scope{arena};
    memory::allocate<char>(arena, 500);
    EXPECT_GT(arena.utilized, before);
  }
  EXPECT_EQ(arena.utilized, before);
  EXPECT_GE(arena.peak, before + 500);
  memory::destroy(arena);
}

TEST(Arena, ResizeAndReleaseMostRecent)
{
  memory::Arena arena;
  memory::create(arena, 1024);
  void* a = memory::allocate_bytes(arena, 16, 8);
  void* b = memory::allocate_bytes(arena, 16, 8);
  EXPECT_FALSE(memory::try_resize_bytes(arena, a, 32));
  EXPECT_TRUE(memory::try_resize_bytes(arena, b, 64));
  const auto used = arena.utilized;
  memory::release_bytes(arena, a, 16);  // not most recent: no-op
  EXPECT_EQ(arena.utilized, used);
  memory::release_bytes(arena, b, 64);
  EXPECT_LT(arena.utilized, used);
  memory::destroy(arena);
}

TEST(ArenaAllocator, VectorLivesInArena)
{
  memory::Arena arena;
  memory::create(arena, 1 << 16);
  auto v = memory::make_vector<int>(arena);
  for (int i = 0; i < 1000; ++i)
  {
    v.push_back(i);
  }
  EXPECT_EQ(v.size(), 1000UL);
  const auto lo = reinterpret_cast<std::uintptr_t>(arena.data);
  const auto p = reinterpret_cast<std::uintptr_t>(v.data());
  EXPECT_GE(p, lo);
  EXPECT_LT(p, lo + arena.capacity);
  memory::destroy(arena);
}

TEST(MemoryDeathTest, ExhaustionAborts)
{
  memory::Arena arena;
  memory::create(arena, 64, "tiny");
  EXPECT_DEATH(memory::allocate<char>(arena, 65), "exhausted");
  memory::destroy(arena);
}

TEST(MemoryDeathTest, PopOutOfOrderAborts)
{
  memory::Arena root;
  memory::create(root, 4096);
  memory::Arena a;
  memory::Arena b;
  memory::push(root, a, 512);
  memory::push(root, b, 512);
  EXPECT_DEATH(memory::pop(root, a), "not the most recently pushed");
  memory::pop(root, b);
  memory::pop(root, a);
  memory::destroy(root);
}
