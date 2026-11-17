/**
 * @copyright 2026-present Brian Cairl
 *
 * @file random.hpp
 *
 * Seeded, deterministic random numbers for procedural generation: the same seed always produces the
 * same level, on every platform. No heap, no syscalls.
 */
#pragma once

// C++ Standard Library
#include <cstdint>

namespace lvl::random
{

/// splitmix64 state
struct State
{
  std::uint64_t value = 0x9E3779B97F4A7C15ULL;
};

/// Creates a generator state from a seed (any seed, including 0, is fine)
constexpr State seeded(std::uint64_t seed) { return State{seed ^ 0x9E3779B97F4A7C15ULL}; }

/// Next 64 random bits
constexpr std::uint64_t next(State& state)
{
  std::uint64_t z = (state.value += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

/// Uniform float in [0, 1)
constexpr float unit(State& state) { return static_cast<float>(next(state) >> 40) * (1.0F / 16777216.0F); }

/// Uniform float in [lo, hi)
constexpr float uniform(State& state, float lo, float hi) { return lo + (hi - lo) * unit(state); }

/// Uniform integer in [0, n) (n > 0)
constexpr std::uint32_t below(State& state, std::uint32_t n)
{
  return static_cast<std::uint32_t>((static_cast<std::uint64_t>(next(state) >> 32) * n) >> 32);
}

/// True with the given probability (0 to 1)
constexpr bool chance(State& state, float probability) { return unit(state) < probability; }

/// Stateless hash of a seed and coordinates (for per-cell randomness independent of visit order)
constexpr std::uint64_t hash(std::uint64_t seed, std::int64_t x, std::int64_t y)
{
  State s{
    seed ^ (static_cast<std::uint64_t>(x) * 0x632BE59BD9B4E019ULL) ^ (static_cast<std::uint64_t>(y) * 0x85157AF5ULL)};
  return next(s);
}

}  // namespace lvl::random
