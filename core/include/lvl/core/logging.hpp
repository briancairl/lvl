/**
 * @copyright 2026-present Brian Cairl
 *
 * @file logging.hpp
 *
 * Severity logging plus the LVL_ASSERT* / LVL_FAIL family. Assertion failures log the failing
 * condition, operand values and a caller-supplied explanation, then <code>std::abort()</code>. They
 * are NOT compiled out in release builds; LVL_DEBUG_ASSERT* is, for expensive checks.
 *
 * Every log statement formats into a thread-local line buffer and is emitted as one write under a
 * lock when the statement ends, so lines from parallel load/save workers never interleave. The most
 * recent lines are also kept in a fixed-capacity history which the editor's log panel displays.
 */
#pragma once

// C++ Standard Library
#include <cstddef>
#include <cstdlib>
#include <ostream>
#include <utility>

// LVL
#include "lvl/core/format.hpp"

namespace lvl
{

enum class LogSeverity
{
  kDebug,
  kInfo,
  kWarn,
  kError,
  kFatal,
  _kN
};

std::ostream& operator<<(std::ostream& os, LogSeverity severity);

struct LogFileInfo
{
  /// Log severity
  LogSeverity severity;
  /// File where log originates
  const char* file;
  /// File line where log originates
  int line;
};

std::ostream& operator<<(std::ostream& os, const LogFileInfo& info);

/// Sets the output stream for all severities; <code>nullptr</code> restores the defaults
void setLogStream(std::ostream* os);

/// Sets the output stream for one severity; <code>nullptr</code> restores its default
void setLogStream(LogSeverity severity, std::ostream* os);

/// Returns the output stream for a severity
std::ostream* getLogStream(LogSeverity severity);

/// Sets the lowest severity which is emitted (default: kDebug in debug builds, kInfo otherwise)
void setLogLevel(LogSeverity severity);

/// Returns the lowest severity which is emitted
LogSeverity getLogLevel();

/// Maximum characters kept per history line (longer lines are truncated in the history only)
inline constexpr std::size_t kLogHistoryLineLength = 256UL;

/// Number of most-recent lines kept in the log history
inline constexpr std::size_t kLogHistoryCapacity = 512UL;

/// One entry in the log history
struct LogHistoryLine
{
  /// Severity the line was logged with
  LogSeverity severity;
  /// Monotonic sequence number of this line (since program start)
  std::size_t sequence;
  /// Null-terminated (possibly truncated) message text
  char text[kLogHistoryLineLength];
};

/**
 * @brief Calls <code>visit(const LogHistoryLine&)</code> for every line in the history, oldest first
 *
 * The history lock is held for the duration of the call; <code>visit</code> must not log.
 */
template <typename VisitorT> void visitLogHistory(VisitorT&& visit);

/// Total number of lines ever added to the history (can be used to detect new lines cheaply)
std::size_t getLogHistorySequence();

/// Removes all lines from the history
void clearLogHistory();

namespace detail
{

/// Locks the history and returns the ring buffer, its capacity-relative start index and its size
struct LogHistoryAccess
{
  LogHistoryAccess();
  ~LogHistoryAccess();
  LogHistoryAccess(const LogHistoryAccess&) = delete;
  LogHistoryAccess& operator=(const LogHistoryAccess&) = delete;

  const LogHistoryLine* lines;
  std::size_t first;
  std::size_t size;
};

}  // namespace detail

template <typename VisitorT> void visitLogHistory(VisitorT&& visit)
{
  const detail::LogHistoryAccess access;
  for (std::size_t i = 0; i < access.size; ++i)
  {
    visit(access.lines[(access.first + i) % kLogHistoryCapacity]);
  }
}

/**
 * @brief One log statement
 *
 * Accumulates streamed values into a thread-local line buffer; emits the whole line (to the
 * severity's stream and the history) on destruction.
 */
class Log
{
public:
  ~Log();

  /// Creates a log which writes to <code>target</code> (no-op if <code>target</code> is null)
  Log(std::ostream* target, LogSeverity severity);

  Log(Log&& other);
  Log& operator=(Log&& other);

  void swap(Log& other);

  constexpr bool is_valid() const { return os_ != nullptr; }
  constexpr operator bool() const { return is_valid(); }

  template <typename T> Log& operator<<(T&& obj)
  {
    if (os_ != nullptr)
    {
      line() << std::forward<T>(obj);
    }
    return *this;
  }

protected:
  /// Thread-local line stream the statement is formatted into
  static std::ostream& line();

  /// Emits the line now (used by Abort before aborting)
  void emit();

private:
  Log(const Log& other) = delete;
  Log& operator=(const Log& other) = delete;

  std::ostream* os_;
  LogSeverity severity_;
};

/// Swallows everything streamed into it (compiled-out debug logs/asserts)
struct NoLog
{
  template <typename T> constexpr NoLog& operator<<([[maybe_unused]] T&& obj) { return *this; }
};

/// A log statement which aborts the program after emitting
class Abort : public Log
{
public:
  [[noreturn]] ~Abort();

  Abort();
  explicit Abort(std::ostream* target);

  Abort(Abort&& other) = delete;
  Abort& operator=(Abort&& other) = delete;

private:
  Abort(const Abort& other) = delete;
  Abort& operator=(const Abort& other) = delete;
};

/// Returns true if logs of <code>severity</code> are emitted
bool isLogEnabled(LogSeverity severity);

}  // namespace lvl

#define __LVL_STR_EXPR(x) #x

#define LVL_LOG_GENERATE_FILE_INFO(severity)                                                                           \
  ::lvl::LogFileInfo { severity, __FILE__, __LINE__ }

#ifdef LVL_LOGGING_DISABLED

#define LVL_LOG(severity)                                                                                              \
  ::lvl::NoLog {}

#else

#define LVL_LOG(severity)                                                                                              \
  ::lvl::Log{::lvl::isLogEnabled(severity) ? ::lvl::getLogStream(severity) : nullptr, severity}                        \
    << "[LVL] (" << LVL_LOG_GENERATE_FILE_INFO(severity) << ") "

#endif  // LVL_LOGGING_DISABLED

#define LVL_LOG_FMT(severity, fmt, ...) LVL_LOG(severity) << ::lvl::format<1024UL>(fmt, __VA_ARGS__)

#define LVL_LOG_DEBUG_FMT(fmt, ...) LVL_LOG_FMT(::lvl::LogSeverity::kDebug, fmt, __VA_ARGS__)
#define LVL_LOG_INFO_FMT(fmt, ...) LVL_LOG_FMT(::lvl::LogSeverity::kInfo, fmt, __VA_ARGS__)
#define LVL_LOG_WARN_FMT(fmt, ...) LVL_LOG_FMT(::lvl::LogSeverity::kWarn, fmt, __VA_ARGS__)
#define LVL_LOG_ERROR_FMT(fmt, ...) LVL_LOG_FMT(::lvl::LogSeverity::kError, fmt, __VA_ARGS__)
#define LVL_LOG_FATAL_FMT(fmt, ...) LVL_LOG_FMT(::lvl::LogSeverity::kFatal, fmt, __VA_ARGS__)

#if defined(NDEBUG) || defined(_NDEBUG)
#define LVL_LOG_DEBUG()                                                                                                \
  ::lvl::NoLog {}
#else
#define LVL_LOG_DEBUG() LVL_LOG(::lvl::LogSeverity::kDebug)
#endif  // defined(NDEBUG) || defined(_NDEBUG)

#define LVL_LOG_INFO() LVL_LOG(::lvl::LogSeverity::kInfo)
#define LVL_LOG_WARN() LVL_LOG(::lvl::LogSeverity::kWarn)
#define LVL_LOG_ERROR() LVL_LOG(::lvl::LogSeverity::kError)
#define LVL_LOG_FATAL() LVL_LOG(::lvl::LogSeverity::kFatal)

#define LVL_FAIL() ::lvl::Abort{} << LVL_LOG_GENERATE_FILE_INFO(::lvl::LogSeverity::kFatal) << "\n\n"

// Unlike gft's GFT_ASSERT, the condition is parenthesized, so compound conditions need no extra parens.
#define LVL_ASSERT(condition)                                                                                          \
  if (!(condition))                                                                                                    \
  LVL_FAIL() << "cond: " << __LVL_STR_EXPR(condition) << "\nexpl: "

// clang-format off
#define LVL_ASSERT_BINARY_OP(lhs, op, rhs)                                                                             \
  if (!((lhs) op (rhs)))                                                                                               \
  LVL_FAIL() << "cond: "                                                                                               \
             << __LVL_STR_EXPR(lhs) << ' ' << __LVL_STR_EXPR(op) << ' ' << __LVL_STR_EXPR(rhs)                         \
             << "\n\nwhere:\n  " << __LVL_STR_EXPR(lhs) << "\n  =\n  " << (lhs) << '\n'                                \
             << "\nwhere:\n  " << __LVL_STR_EXPR(rhs) << "\n  =\n  " << (rhs) << '\n'                                  \
             << "\nexpl: "
// clang-format on

#define LVL_ASSERT_NULL(val_ptr) LVL_ASSERT((val_ptr) == nullptr)
#define LVL_ASSERT_NON_NULL(val_ptr) LVL_ASSERT((val_ptr) != nullptr)
#define LVL_ASSERT_TRUE(val_bool) LVL_ASSERT(static_cast<bool>(val_bool))
#define LVL_ASSERT_FALSE(val_bool) LVL_ASSERT(!static_cast<bool>(val_bool))
#define LVL_ASSERT_EQ(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, ==, val_rhs)
#define LVL_ASSERT_NE(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, !=, val_rhs)
#define LVL_ASSERT_LT(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, <, val_rhs)
#define LVL_ASSERT_LE(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, <=, val_rhs)
#define LVL_ASSERT_GT(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, >, val_rhs)
#define LVL_ASSERT_GE(val_lhs, val_rhs) LVL_ASSERT_BINARY_OP(val_lhs, >=, val_rhs)

#if defined(NDEBUG) || defined(_NDEBUG)

#define LVL_DEBUG_ASSERT(condition)                                                                                    \
  ::lvl::NoLog {}
#define LVL_DEBUG_ASSERT_BINARY_OP(lhs, op, rhs)                                                                       \
  ::lvl::NoLog {}

#else

#define LVL_DEBUG_ASSERT(condition)                                                                                    \
  if (!(condition))                                                                                                    \
  LVL_FAIL() << "<=========== DEBUG MODE ENABLED =============>\n\ncond: " << __LVL_STR_EXPR(condition) << "\nexpl: "

#define LVL_DEBUG_ASSERT_BINARY_OP(lhs, op, rhs) LVL_ASSERT_BINARY_OP(lhs, op, rhs)

#endif  // defined(NDEBUG) || defined(_NDEBUG)

#define LVL_DEBUG_ASSERT_NULL(val_ptr) LVL_DEBUG_ASSERT((val_ptr) == nullptr)
#define LVL_DEBUG_ASSERT_NON_NULL(val_ptr) LVL_DEBUG_ASSERT((val_ptr) != nullptr)
#define LVL_DEBUG_ASSERT_TRUE(val_bool) LVL_DEBUG_ASSERT(static_cast<bool>(val_bool))
#define LVL_DEBUG_ASSERT_FALSE(val_bool) LVL_DEBUG_ASSERT(!static_cast<bool>(val_bool))
#define LVL_DEBUG_ASSERT_EQ(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, ==, val_rhs)
#define LVL_DEBUG_ASSERT_NE(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, !=, val_rhs)
#define LVL_DEBUG_ASSERT_LT(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, <, val_rhs)
#define LVL_DEBUG_ASSERT_LE(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, <=, val_rhs)
#define LVL_DEBUG_ASSERT_GT(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, >, val_rhs)
#define LVL_DEBUG_ASSERT_GE(val_lhs, val_rhs) LVL_DEBUG_ASSERT_BINARY_OP(val_lhs, >=, val_rhs)

#define LVL_UNREACHABLE() __builtin_unreachable()

#define LVL_SHOULD_NEVER_HAPPEN(reason)                                                                                \
  LVL_FAIL() << reason;                                                                                                \
  LVL_UNREACHABLE();

#define LVL_OS_ENUM_CASE(e)                                                                                            \
  case e: {                                                                                                            \
    return os << __LVL_STR_EXPR(e);                                                                                    \
  }

#define LVL_OSNV(x) '[' << __LVL_STR_EXPR(x) << "=" << (x) << ']'
