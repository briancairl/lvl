/**
 * @copyright 2026-present Brian Cairl
 *
 * @file memory.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cstdlib>
#include <ostream>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/core/memory.hpp"

namespace lvl::memory
{
namespace
{

constexpr std::size_t kSubArenaAlignment = 64UL;

std::size_t offset_of(const Arena& arena, const void* p)
{
  return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(arena.data));
}

}  // namespace

void initialize(Arena& arena, void* p, std::size_t capacity, const char* label)
{
  LVL_ASSERT_NULL(arena.data) << "Arena '" << arena.label << "' is already initialized";
  LVL_ASSERT_NON_NULL(p);
  arena.data = p;
  arena.utilized = 0UL;
  arena.capacity = capacity;
  arena.peak = 0UL;
  arena.last = 0UL;
  arena.label = label;
}

void reset(Arena& arena)
{
  LVL_ASSERT(is_valid(arena));
  arena = Arena{};
}

void create(Arena& arena, std::size_t capacity, const char* label)
{
  LVL_ASSERT(!is_valid(arena)) << "Arena '" << arena.label << "' already created";
  // Aligned (and capacity rounded up) so sub-arenas pushed in sequence need no padding between them
  capacity = align_address_forward(capacity, kSubArenaAlignment);
  void* const p = std::aligned_alloc(kSubArenaAlignment, capacity);
  LVL_ASSERT_NON_NULL(p) << "Failed to reserve " << Bytes{capacity} << " for arena '" << label << "'";
  initialize(arena, p, capacity, label);
  LVL_LOG_DEBUG() << "Created " << arena;
}

void destroy(Arena& arena)
{
  LVL_ASSERT(is_valid(arena));
  LVL_LOG_DEBUG() << "Destroying " << arena;
  std::free(arena.data);
  reset(arena);
}

void push(Arena& arena, Arena& child, std::size_t capacity, const char* label)
{
  LVL_ASSERT(is_valid(arena));
  LVL_ASSERT(!is_valid(child));
  LVL_ASSERT_LE(capacity, available(arena))
    << "Arena '" << arena.label << "' cannot fit sub-arena '" << label << "' of " << Bytes{capacity};
  // Keep sub-arenas (and so their contents) aligned; capacities are rounded up to keep the next one aligned
  capacity = align_address_forward(capacity, kSubArenaAlignment);
  const auto start =
    align_address_forward(reinterpret_cast<std::uintptr_t>(arena.data) + arena.utilized, kSubArenaAlignment);
  const std::size_t padding = start - (reinterpret_cast<std::uintptr_t>(arena.data) + arena.utilized);
  LVL_ASSERT_LE(capacity + padding, available(arena));
  initialize(child, reinterpret_cast<void*>(start), capacity, label);
  arena.last = arena.utilized;
  arena.utilized += capacity + padding;
  arena.peak = std::max(arena.peak, arena.utilized);
}

void push(Arena& arena, Arena& child, const char* label)
{
  LVL_ASSERT(is_valid(arena));
  LVL_ASSERT_GT(available(arena), 2UL * kSubArenaAlignment);
  push(arena, child, available(arena) - 2UL * kSubArenaAlignment, label);
}

void pop(Arena& arena, Arena& child)
{
  LVL_ASSERT(is_valid(arena));
  LVL_ASSERT(is_valid(child));
  const std::size_t end = offset_of(arena, child.data) + child.capacity;
  LVL_ASSERT_EQ(end, arena.utilized) << "Sub-arena '" << child.label << "' is not the most recently pushed";
  arena.utilized = offset_of(arena, child.data);
  arena.last = arena.utilized;
  reset(child);
}

void clear(Arena& arena)
{
  LVL_ASSERT(is_valid(arena));
  arena.utilized = 0;
  arena.last = 0;
}

void* allocate_bytes(Arena& arena, std::size_t size, std::size_t alignment)
{
  LVL_ASSERT(is_valid(arena)) << "Allocation from uninitialized arena '" << arena.label << "'";
  const auto base = reinterpret_cast<std::uintptr_t>(arena.data);
  const auto aligned = align_address_forward(base + arena.utilized, alignment);
  const std::size_t offset = aligned - base;
  LVL_ASSERT_LE(offset + size, arena.capacity)
    << "Arena '" << arena.label << "' exhausted requesting " << Bytes{size} << " (" << arena << ")";
  arena.last = offset;
  arena.utilized = offset + size;
  arena.peak = std::max(arena.peak, arena.utilized);
  return reinterpret_cast<void*>(aligned);
}

bool try_resize_bytes(Arena& arena, void* p, std::size_t new_size)
{
  if (p == nullptr or !is_valid(arena))
  {
    return false;
  }
  const std::size_t offset = offset_of(arena, p);
  if (offset != arena.last or offset + new_size > arena.capacity)
  {
    return false;
  }
  arena.utilized = offset + new_size;
  arena.peak = std::max(arena.peak, arena.utilized);
  return true;
}

void release_bytes(Arena& arena, void* p, [[maybe_unused]] std::size_t size)
{
  if (p == nullptr or !is_valid(arena))
  {
    return;
  }
  const std::size_t offset = offset_of(arena, p);
  if (offset == arena.last and offset < arena.utilized)
  {
    arena.utilized = offset;
  }
}

Scope::Scope(Arena& arena) : arena_{&arena}, mark_{arena.utilized}, last_{arena.last} {}

Scope::~Scope()
{
  LVL_ASSERT_GE(arena_->utilized, mark_) << "Arena '" << arena_->label << "' rewound past an open Scope";
  arena_->utilized = mark_;
  arena_->last = last_;
}

std::ostream& operator<<(std::ostream& os, const Bytes& bytes)
{
  constexpr std::size_t kKiB = 1024UL;
  constexpr std::size_t kMiB = kKiB * 1024UL;
  if (bytes.value >= kMiB)
  {
    return os << format("%.2f MiB", static_cast<double>(bytes.value) / static_cast<double>(kMiB));
  }
  if (bytes.value >= kKiB)
  {
    return os << format("%.2f KiB", static_cast<double>(bytes.value) / static_cast<double>(kKiB));
  }
  return os << bytes.value << " B";
}

std::ostream& operator<<(std::ostream& os, const Arena& arena)
{
  return os << "arena '" << arena.label << "' { used: " << Bytes{arena.utilized} << ", peak: " << Bytes{arena.peak}
            << ", capacity: " << Bytes{arena.capacity} << " }";
}

}  // namespace lvl::memory
