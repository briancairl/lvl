/**
 * @copyright 2026-present Brian Cairl
 *
 * @file logging.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <streambuf>

// LVL
#include "lvl/core/logging.hpp"

namespace lvl
{
namespace
{

constexpr std::size_t kSeverityCount = static_cast<std::size_t>(LogSeverity::_kN);

std::ostream* const kLogStreamDefaults[kSeverityCount] = {
  std::addressof(std::cout),
  std::addressof(std::cout),
  std::addressof(std::cout),
  std::addressof(std::cerr),
  std::addressof(std::cerr)};

std::ostream* log_streams[kSeverityCount] = {
  std::addressof(std::cout),
  std::addressof(std::cout),
  std::addressof(std::cout),
  std::addressof(std::cerr),
  std::addressof(std::cerr)};

#if defined(NDEBUG) || defined(_NDEBUG)
std::atomic<int> log_level{static_cast<int>(LogSeverity::kInfo)};
#else
std::atomic<int> log_level{static_cast<int>(LogSeverity::kDebug)};
#endif  // defined(NDEBUG) || defined(_NDEBUG)

/// Serializes emitted lines and guards the history
std::mutex& emitMutex()
{
  static std::mutex m;
  return m;
}

/// Fixed-capacity ring of recent lines (statically allocated; no heap)
struct History
{
  LogHistoryLine lines[kLogHistoryCapacity];
  std::size_t first = 0;
  std::size_t size = 0;
  std::size_t sequence = 0;
};

History& history()
{
  static History h;
  return h;
}

/// Fixed-size, truncating stream buffer which one log statement is formatted into
class LineBuffer : public std::streambuf
{
public:
  static constexpr std::size_t kCapacity = 8192UL;

  LineBuffer() { reset(); }

  void reset() { this->setp(buffer_, buffer_ + kCapacity); }

  const char* data() const { return buffer_; }

  std::size_t size() const { return static_cast<std::size_t>(this->pptr() - this->pbase()); }

protected:
  /// Buffer full: drop further characters (the line is truncated rather than allocated for)
  int_type overflow(int_type ch) override { return traits_type::not_eof(ch); }

private:
  char buffer_[kCapacity];
};

struct ThreadLine
{
  LineBuffer buffer;
  std::ostream stream{&buffer};
};

ThreadLine& threadLine()
{
  thread_local ThreadLine tl;
  return tl;
}

void appendToHistory(LogSeverity severity, const char* text, std::size_t len)
{
  auto& h = history();
  const std::size_t index = (h.first + h.size) % kLogHistoryCapacity;
  if (h.size == kLogHistoryCapacity)
  {
    h.first = (h.first + 1) % kLogHistoryCapacity;
  }
  else
  {
    ++h.size;
  }
  auto& line = h.lines[index];
  line.severity = severity;
  line.sequence = h.sequence++;
  const std::size_t n = std::min(len, kLogHistoryLineLength - 1);
  std::memcpy(line.text, text, n);
  line.text[n] = '\0';
}

}  // namespace

void setLogStream(LogSeverity severity, std::ostream* os)
{
  LVL_ASSERT_NE(severity, LogSeverity::_kN);
  const auto i = static_cast<std::size_t>(severity);
  const std::lock_guard lock{emitMutex()};
  log_streams[i] = (os == nullptr) ? kLogStreamDefaults[i] : os;
}

void setLogStream(std::ostream* os)
{
  const std::lock_guard lock{emitMutex()};
  for (std::size_t i = 0; i < kSeverityCount; ++i)
  {
    log_streams[i] = (os == nullptr) ? kLogStreamDefaults[i] : os;
  }
}

std::ostream* getLogStream(LogSeverity severity)
{
  LVL_ASSERT_NE(severity, LogSeverity::_kN);
  return log_streams[static_cast<std::size_t>(severity)];
}

void setLogLevel(LogSeverity severity)
{
  LVL_ASSERT_NE(severity, LogSeverity::_kN);
  log_level.store(static_cast<int>(severity));
}

LogSeverity getLogLevel() { return static_cast<LogSeverity>(log_level.load()); }

bool isLogEnabled(LogSeverity severity) { return static_cast<int>(severity) >= log_level.load(); }

std::size_t getLogHistorySequence()
{
  const std::lock_guard lock{emitMutex()};
  return history().sequence;
}

void clearLogHistory()
{
  const std::lock_guard lock{emitMutex()};
  history().first = 0;
  history().size = 0;
}

namespace detail
{

LogHistoryAccess::LogHistoryAccess()
{
  emitMutex().lock();
  lines = history().lines;
  first = history().first;
  size = history().size;
}

LogHistoryAccess::~LogHistoryAccess() { emitMutex().unlock(); }

}  // namespace detail

std::ostream& operator<<(std::ostream& os, LogSeverity severity)
{
  switch (severity)
  {
  case LogSeverity::kDebug:
    return os << "Debug";
  case LogSeverity::kInfo:
    return os << "Info";
  case LogSeverity::kWarn:
    return os << "Warn";
  case LogSeverity::kError:
    return os << "Error";
  case LogSeverity::kFatal:
    return os << "Fatal";
  case LogSeverity::_kN:
    break;
  }
  return os << "<invalid LogSeverity>";
}

std::ostream& operator<<(std::ostream& os, const LogFileInfo& info)
{
  return os << info.severity << ":" << info.file << ':' << info.line;
}

std::ostream& Log::line() { return threadLine().stream; }

Log::~Log() { this->emit(); }

Log::Log(std::ostream* target, LogSeverity severity) : os_{target}, severity_{severity}
{
  if (os_ != nullptr)
  {
    threadLine().buffer.reset();
  }
}

Log::Log(Log&& other) : os_{nullptr}, severity_{LogSeverity::kInfo} { this->swap(other); }

Log& Log::operator=(Log&& other)
{
  this->swap(other);
  return *this;
}

void Log::swap(Log& other)
{
  std::swap(this->os_, other.os_);
  std::swap(this->severity_, other.severity_);
}

void Log::emit()
{
  if (os_ == nullptr)
  {
    return;
  }
  auto& tl = threadLine();
  {
    const std::lock_guard lock{emitMutex()};
    os_->write(tl.buffer.data(), static_cast<std::streamsize>(tl.buffer.size()));
    os_->put('\n');
    os_->flush();
    appendToHistory(severity_, tl.buffer.data(), tl.buffer.size());
  }
  tl.buffer.reset();
  os_ = nullptr;
}

Abort::~Abort()
{
  (*this) << "\n\n********** RUNTIME ASSERTION FAILED **********\n";
  this->emit();
  std::abort();
}

Abort::Abort(std::ostream* target) : Log{target, LogSeverity::kFatal}
{
  (*this) << "********** RUNTIME ASSERTION FAILED **********\n\n";
}

Abort::Abort() : Abort{std::addressof(std::cerr)} {}

}  // namespace lvl
