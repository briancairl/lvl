/**
 * @copyright 2026-present Brian Cairl
 *
 * @file memory.hpp
 *
 * Arena (bump) allocation. The editor makes one <code>create()</code> (one malloc) for its whole
 * working set and carves fixed-capacity sub-arenas out of it with <code>push()</code>: one for the
 * level document, one for tilesets, one for scratch work, one per background job, ... Exhausting an
 * arena is an assert failure; nothing silently falls back to the heap.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

// LVL
#include "lvl/core/logging.hpp"

namespace lvl::memory
{

struct Arena
{
  /// Pointer to start of arena
  void* data = nullptr;
  /// Arena utilization, in bytes
  std::size_t utilized = 0;
  /// Arena capacity, in bytes
  std::size_t capacity = 0;
  /// Highest utilization reached, in bytes (for sizing arenas)
  std::size_t peak = 0;
  /// Offset of the most recent allocation (lets the newest block be grown/released in place)
  std::size_t last = 0;
  /// Human-readable label, used in exhaustion messages
  const char* label = "arena";
};

/// Alignment of an arena's memory and of every sub-arena carved from it
inline constexpr std::size_t kSubArenaAlignment = 64UL;

constexpr bool is_valid(const Arena& arena) { return (arena.data != nullptr) && (arena.capacity > 0); }

/// Bytes still available
constexpr std::size_t available(const Arena& arena) { return arena.capacity - arena.utilized; }

/// Wraps caller-owned memory <code>p</code> as an arena (does not take ownership)
void initialize(Arena& arena, void* p, std::size_t capacity, const char* label = "arena");

/// Invalidates an arena without freeing anything
void reset(Arena& arena);

/// Allocates the arena's memory: the ONE heap allocation made for it
void create(Arena& arena, std::size_t capacity, const char* label = "arena");

/// Frees memory allocated by <code>create</code>
void destroy(Arena& arena);

/// Carves a fixed-capacity sub-arena <code>child</code> out of <code>arena</code>
void push(Arena& arena, Arena& child, std::size_t capacity, const char* label = "arena");

/// Carves all remaining space of <code>arena</code> into <code>child</code>
void push(Arena& arena, Arena& child, const char* label = "arena");

/// Returns the most recently pushed sub-arena <code>child</code> to <code>arena</code>
void pop(Arena& arena, Arena& child);

/// Releases everything allocated from <code>arena</code> (keeps its memory)
void clear(Arena& arena);

constexpr std::uintptr_t align_address_mod(std::uintptr_t ptr_int, std::uintptr_t alignment)
{
  return ptr_int & (alignment - 1);
}

constexpr std::uintptr_t align_address_forward(std::uintptr_t ptr_int, std::uintptr_t alignment)
{
  if (const std::uintptr_t mod = align_address_mod(ptr_int, alignment); mod != 0)
  {
    ptr_int += alignment - mod;
  }
  return ptr_int;
}

/// Allocates <code>size</code> bytes aligned to <code>alignment</code>
void* allocate_bytes(Arena& arena, std::size_t size, std::size_t alignment);

/**
 * @brief Attempts to resize the block at <code>p</code> in place
 *
 * Succeeds only if <code>p</code> is the most recent allocation and the arena has room.
 */
bool try_resize_bytes(Arena& arena, void* p, std::size_t new_size);

/// Gives back the block at <code>p</code> if it is the most recent allocation; otherwise a no-op
void release_bytes(Arena& arena, void* p, std::size_t size);

/// Allocates (uninitialized) storage for <code>count</code> objects of <code>T</code>
template <typename T> T* allocate(Arena& arena, std::size_t count)
{
  return reinterpret_cast<T*>(allocate_bytes(arena, count * sizeof(T), alignof(T)));
}

/// Allocates storage for <code>count</code> objects of <code>T</code>, each initialized to <code>init</code>
template <typename T> T* allocate(Arena& arena, std::size_t count, const T& init)
{
  T* ptr = allocate<T>(arena, count);
  for (std::size_t i = 0; i < count; ++i)
  {
    new (ptr + i) T{init};
  }
  return ptr;
}

/// Copies <code>count</code> trivially-copyable objects into the arena
template <typename T> T* duplicate(Arena& arena, const T* src, std::size_t count)
{
  static_assert(std::is_trivially_copyable_v<T>);
  T* ptr = allocate<T>(arena, count);
  std::memcpy(static_cast<void*>(ptr), src, count * sizeof(T));
  return ptr;
}

/**
 * @brief Marks an arena on construction and rewinds it to the mark on destruction
 *
 * For scratch work: everything allocated inside the scope is released when it ends. Scopes on the
 * same arena must be strictly nested.
 */
class Scope
{
public:
  explicit Scope(Arena& arena);
  ~Scope();

  Scope(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope& operator=(Scope&&) = delete;

private:
  Arena* arena_;
  std::size_t mark_;
  std::size_t last_;
};

/**
 * @brief Standard-library allocator drawing from an Arena
 *
 * Stateful (holds the arena pointer). <code>deallocate</code> only reclaims the most recent block, so a
 * growing container usually extends in place; otherwise the old block is reclaimed when the arena
 * is cleared. Used for document containers and the EnTT registry.
 */
template <typename T> class ArenaAllocator
{
public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::true_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;

  ArenaAllocator() = default;
  explicit ArenaAllocator(Arena& arena) noexcept : arena_{&arena} {}

  template <typename U> ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_{other.arena()} {}

  [[nodiscard]] T* allocate(std::size_t n)
  {
    LVL_ASSERT_NON_NULL(arena_) << "ArenaAllocator used without an arena";
    return memory::allocate<T>(*arena_, n);
  }

  void deallocate(T* p, std::size_t n) noexcept
  {
    if (arena_ != nullptr)
    {
      release_bytes(*arena_, p, n * sizeof(T));
    }
  }

  [[nodiscard]] Arena* arena() const noexcept { return arena_; }

  template <typename U> bool operator==(const ArenaAllocator<U>& other) const noexcept
  {
    return arena_ == other.arena();
  }
  template <typename U> bool operator!=(const ArenaAllocator<U>& other) const noexcept { return !(*this == other); }

private:
  Arena* arena_ = nullptr;
};

/// <code>std::vector</code> whose storage lives in an Arena
template <typename T> using Vector = std::vector<T, ArenaAllocator<T>>;

/// Creates an empty arena-backed vector
template <typename T> Vector<T> make_vector(Arena& arena) { return Vector<T>{ArenaAllocator<T>{arena}}; }

/// Creates an arena-backed vector of <code>n</code> copies of <code>value</code>
template <typename T> Vector<T> make_vector(Arena& arena, std::size_t n, const T& value = T{})
{
  return Vector<T>(n, value, ArenaAllocator<T>{arena});
}

/// Bytes, for formatting arena sizes in logs: <code>LVL_LOG_INFO() << Bytes{n}</code>
struct Bytes
{
  std::size_t value;
};

std::ostream& operator<<(std::ostream& os, const Bytes& bytes);

std::ostream& operator<<(std::ostream& os, const Arena& arena);

}  // namespace lvl::memory
