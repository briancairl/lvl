// C++ Standard Library
#include <atomic>
#include <vector>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/jobs.hpp"
#include "lvl/core/random.hpp"

using namespace lvl;

TEST(Jobs, ParallelForVisitsEveryIndexOnce)
{
  constexpr std::size_t kCount = 10000;
  std::vector<std::atomic<int>> visits(kCount);
  std::atomic<std::size_t> max_worker{0};
  jobs::parallel_for(kCount, 4, [&](std::size_t i, std::size_t worker) {
    visits[i].fetch_add(1);
    std::size_t seen = max_worker.load();
    while (worker > seen and !max_worker.compare_exchange_weak(seen, worker))
    {
    }
  });
  for (const auto& v : visits)
  {
    ASSERT_EQ(v.load(), 1);
  }
  EXPECT_LT(max_worker.load(), 4UL);
}

TEST(Jobs, ParallelForHandlesEmptyAndSingle)
{
  int calls = 0;
  jobs::parallel_for(0, 8, [&](std::size_t, std::size_t) { ++calls; });
  EXPECT_EQ(calls, 0);
  jobs::parallel_for(1, 8, [&](std::size_t, std::size_t worker) {
    ++calls;
    EXPECT_EQ(worker, 0UL);
  });
  EXPECT_EQ(calls, 1);
}

namespace
{

struct SumJob
{
  std::size_t n = 0;
  std::size_t result = 0;

  void run(jobs::Progress& progress)
  {
    progress.reset(n);
    progress.stage("summing");
    std::atomic<std::size_t> sum{0};
    jobs::parallel_for(n, 4, [&](std::size_t i, std::size_t) {
      sum.fetch_add(i);
      progress.advance();
    });
    result = sum.load();
  }
};

struct CancellableJob
{
  bool stopped_early = false;

  void run(jobs::Progress& progress)
  {
    progress.reset(1000000);
    for (std::size_t i = 0; i < 1000000; ++i)
    {
      if (progress.cancelled())
      {
        stopped_early = true;
        return;
      }
      std::this_thread::yield();
      progress.advance();
    }
  }
};

}  // namespace

TEST(Jobs, BackgroundRunsAndReportsProgress)
{
  jobs::Background<SumJob> bg{SumJob{.n = 1000}};
  bg.start();
  while (!bg.done())
  {
    std::this_thread::yield();
  }
  EXPECT_EQ(bg.job().result, 1000UL * 999UL / 2UL);
  EXPECT_FLOAT_EQ(bg.progress().fraction(), 1.0F);
  EXPECT_EQ(bg.progress().stage(), std::string_view{"summing"});
}

TEST(Jobs, BackgroundCanBeCancelled)
{
  jobs::Background<CancellableJob> bg;
  bg.start();
  bg.progress().cancel();
  bg.join();
  EXPECT_TRUE(bg.done());
  EXPECT_TRUE(bg.job().stopped_early);
}

TEST(Random, DeterministicPerSeed)
{
  auto a = random::seeded(42);
  auto b = random::seeded(42);
  auto c = random::seeded(43);
  bool differs = false;
  for (int i = 0; i < 100; ++i)
  {
    const auto x = random::next(a);
    EXPECT_EQ(x, random::next(b));
    differs = differs or (x != random::next(c));
  }
  EXPECT_TRUE(differs);
}

TEST(Random, RangesAreRespected)
{
  auto s = random::seeded(7);
  for (int i = 0; i < 10000; ++i)
  {
    const float u = random::uniform(s, -2.0F, 3.0F);
    EXPECT_GE(u, -2.0F);
    EXPECT_LT(u, 3.0F);
    EXPECT_LT(random::below(s, 17), 17U);
  }
}
