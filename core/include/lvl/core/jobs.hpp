/**
 * @copyright 2026-present Brian Cairl
 *
 * @file jobs.hpp
 *
 * Parallelism for long operations (load/save/export/generation) with progress reporting.
 *
 * - <code>parallel_for</code> splits independent items (layers, image rows, files) across threads.
 * - <code>Background<JobT></code> runs one job off the UI thread while the UI shows its Progress.
 *
 * Jobs are concrete types with a <code>void run(Progress&)</code> member: there is no type-erased task
 * queue (no std::function / virtual dispatch). The editor holds its possible background jobs in a
 * std::variant.
 *
 * Builds without threads (Emscripten without pthreads) run everything synchronously on the calling
 * thread, with the same interfaces.
 */
#pragma once

// C++ Standard Library
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/logging.hpp"

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define LVL_HAS_THREADS 1
#else
#define LVL_HAS_THREADS 0
#endif

namespace lvl::jobs
{

/// True when this build can run work on other threads
inline constexpr bool kHasThreads = (LVL_HAS_THREADS != 0);

/// Number of worker threads worth using (at least 1)
[[nodiscard]] std::size_t hardware_concurrency();

/**
 * @brief Progress of a (possibly multi-threaded) job, readable from the UI thread
 *
 * Work is counted in abstract "steps": set <code>total</code> once, then <code>advance()</code> from any
 * thread as steps complete. The current stage is a short description shown in progress dialogs.
 */
class Progress
{
public:
  /// Resets to zero completed out of <code>total</code> steps
  void reset(std::size_t total)
  {
    total_.store(std::max<std::size_t>(total, 1));
    completed_.store(0);
  }

  /// Adds <code>total</code> more steps to the job (e.g. once the number of layers is known)
  void add_total(std::size_t steps) { total_.fetch_add(steps); }

  /// Marks <code>steps</code> steps as completed (thread-safe)
  void advance(std::size_t steps = 1) { completed_.fetch_add(steps); }

  /// Completed fraction in [0, 1]
  [[nodiscard]] float fraction() const
  {
    const auto total = total_.load();
    return std::min(1.0F, static_cast<float>(completed_.load()) / static_cast<float>(std::max<std::size_t>(total, 1)));
  }

  [[nodiscard]] std::size_t completed() const { return completed_.load(); }
  [[nodiscard]] std::size_t total() const { return total_.load(); }

  /// Sets the stage description (thread-safe)
  void stage(const char* description)
  {
    const std::lock_guard lock{mutex_};
    stage_.assign(description);
  }

  /// Copy of the stage description (thread-safe)
  [[nodiscard]] FixedString<128> stage() const
  {
    const std::lock_guard lock{mutex_};
    return stage_;
  }

  /// Requests the job to stop early; jobs poll <code>cancelled()</code> between steps
  void cancel() { cancelled_.store(true); }

  [[nodiscard]] bool cancelled() const { return cancelled_.load(); }

private:
  std::atomic<std::size_t> completed_{0};
  std::atomic<std::size_t> total_{1};
  std::atomic<bool> cancelled_{false};
  mutable std::mutex mutex_;
  FixedString<128> stage_;
};

/**
 * @brief Calls <code>fn(index, worker)</code> for every <code>index</code> in [0, count) on up to
 *        <code>workers</code> threads
 *
 * Indices are claimed dynamically (an atomic counter), so items of uneven cost balance out.
 * <code>worker</code> is in [0, workers) and identifies the calling thread, e.g. to pick a per-worker
 * scratch arena. The calling thread participates as worker 0. Returns once every item is done.
 */
template <typename Fn> void parallel_for(std::size_t count, std::size_t workers, Fn&& fn)
{
  if (count == 0)
  {
    return;
  }
  workers = std::clamp<std::size_t>(workers, 1, count);
  if constexpr (!kHasThreads)
  {
    workers = 1;
  }

  std::atomic<std::size_t> next{0};
  const auto work = [&next, count, &fn](std::size_t worker) {
    for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1))
    {
      fn(i, worker);
    }
  };

  if (workers == 1)
  {
    work(0);
    return;
  }

#if LVL_HAS_THREADS
  // Worker threads are created per call: these are coarse operations (a whole save/load), so a
  // persistent pool would buy little and would need type-erased task storage.
  std::optional<std::thread> threads[64];
  workers = std::min<std::size_t>(workers, 64);
  for (std::size_t w = 1; w < workers; ++w)
  {
    threads[w].emplace(work, w);
  }
  work(0);
  for (std::size_t w = 1; w < workers; ++w)
  {
    threads[w]->join();
  }
#endif  // LVL_HAS_THREADS
}

/**
 * @brief Runs one job of type <code>JobT</code> on a background thread
 *
 * <code>JobT</code> needs <code>void run(Progress&)</code>; its results stay in the job object, which
 * the owner reads after <code>done()</code>. Not movable once constructed (the thread refers to it), so
 * keep it in place, e.g. in a std::optional or std::variant.
 */
template <typename JobT> class Background
{
public:
  template <typename... Args> explicit Background(Args&&... args) : job_{std::forward<Args>(args)...} {}

  ~Background()
  {
    if (running_.load())
    {
      LVL_LOG_WARN() << "Background job still running at destruction; cancelling and waiting";
      progress_.cancel();
    }
    join();
  }

  Background(const Background&) = delete;
  Background(Background&&) = delete;
  Background& operator=(const Background&) = delete;
  Background& operator=(Background&&) = delete;

  /// Starts the job (runs it to completion right here when threads are unavailable)
  void start()
  {
    LVL_ASSERT_FALSE(started_) << "Background job started twice";
    started_ = true;
    running_.store(true);
#if LVL_HAS_THREADS
    thread_.emplace([this] {
      job_.run(progress_);
      running_.store(false);
    });
#else
    job_.run(progress_);
    running_.store(false);
#endif  // LVL_HAS_THREADS
  }

  /// True once the job has finished (joins the thread the first time it returns true)
  [[nodiscard]] bool done()
  {
    if (started_ and !running_.load())
    {
      join();
      return true;
    }
    return false;
  }

  /// Blocks until the job finishes
  void join()
  {
#if LVL_HAS_THREADS
    if (thread_.has_value() and thread_->joinable())
    {
      thread_->join();
    }
#endif  // LVL_HAS_THREADS
  }

  [[nodiscard]] Progress& progress() { return progress_; }
  [[nodiscard]] const Progress& progress() const { return progress_; }

  /// The job (only safe to touch from the owning thread once <code>done()</code>)
  [[nodiscard]] JobT& job() { return job_; }

private:
  JobT job_;
  Progress progress_;
  bool started_ = false;
  std::atomic<bool> running_{false};
#if LVL_HAS_THREADS
  std::optional<std::thread> thread_;
#endif  // LVL_HAS_THREADS
};

}  // namespace lvl::jobs
