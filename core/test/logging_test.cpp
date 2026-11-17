// C++ Standard Library
#include <atomic>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/logging.hpp"
#include "lvl/core/status.hpp"
#include "lvl/core/visitor.hpp"

namespace
{

struct LogCapture
{
  std::ostringstream os;
  LogCapture() { lvl::setLogStream(&os); }
  ~LogCapture() { lvl::setLogStream(nullptr); }
};

}  // namespace

TEST(Logging, WritesOneLinePerStatement)
{
  LogCapture capture;
  LVL_LOG_INFO() << "hello " << 42;
  LVL_LOG_WARN_FMT("value=%d", 7);
  const std::string text = capture.os.str();
  EXPECT_NE(text.find("hello 42\n"), std::string::npos);
  EXPECT_NE(text.find("value=7\n"), std::string::npos);
  EXPECT_NE(text.find("Info:"), std::string::npos);
  EXPECT_NE(text.find("Warn:"), std::string::npos);
}

TEST(Logging, RespectsLogLevel)
{
  LogCapture capture;
  const auto previous = lvl::getLogLevel();
  lvl::setLogLevel(lvl::LogSeverity::kError);
  LVL_LOG_INFO() << "filtered";
  LVL_LOG_ERROR() << "kept";
  lvl::setLogLevel(previous);
  EXPECT_EQ(capture.os.str().find("filtered"), std::string::npos);
  EXPECT_NE(capture.os.str().find("kept"), std::string::npos);
}

TEST(Logging, HistoryKeepsRecentLines)
{
  LogCapture capture;
  lvl::clearLogHistory();
  const auto before = lvl::getLogHistorySequence();
  for (int i = 0; i < 3; ++i)
  {
    LVL_LOG_INFO() << "history line " << i;
  }
  EXPECT_EQ(lvl::getLogHistorySequence(), before + 3);
  std::vector<std::string> lines;
  lvl::visitLogHistory([&](const lvl::LogHistoryLine& line) { lines.emplace_back(line.text); });
  ASSERT_EQ(lines.size(), 3UL);
  EXPECT_NE(lines.back().find("history line 2"), std::string::npos);
}

TEST(Logging, HistoryIsBounded)
{
  LogCapture capture;
  lvl::clearLogHistory();
  for (std::size_t i = 0; i < lvl::kLogHistoryCapacity + 10; ++i)
  {
    LVL_LOG_INFO() << "line " << i;
  }
  std::size_t count = 0;
  std::string first;
  lvl::visitLogHistory([&](const lvl::LogHistoryLine& line) {
    if (count++ == 0)
    {
      first = line.text;
    }
  });
  EXPECT_EQ(count, lvl::kLogHistoryCapacity);
  EXPECT_NE(first.find("line 10"), std::string::npos);
}

TEST(Logging, LinesFromThreadsDoNotInterleave)
{
  LogCapture capture;
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
  {
    threads.emplace_back([t] {
      for (int i = 0; i < 200; ++i)
      {
        LVL_LOG_INFO() << "thread=" << t << " part-a part-b part-c end";
      }
    });
  }
  for (auto& th : threads)
  {
    th.join();
  }
  std::istringstream lines{capture.os.str()};
  std::string line;
  int count = 0;
  while (std::getline(lines, line))
  {
    ++count;
    EXPECT_NE(line.find("part-a part-b part-c end"), std::string::npos) << line;
  }
  EXPECT_EQ(count, 800);
}

void assertEqual(int a, int b) { LVL_ASSERT_EQ(a, b) << "they differ"; }

void fail() { LVL_FAIL() << "boom"; }

void assertCompound(int a, int b) { LVL_ASSERT(a > 0 and b < 0) << "compound"; }

TEST(LoggingDeathTest, AssertPrintsConditionAndOperands)
{
  EXPECT_DEATH(assertEqual(1, 2), "cond: a == b");
  EXPECT_DEATH(assertCompound(1, 2), "cond: a > 0 and b < 0");
  EXPECT_DEATH(fail(), "boom");
}

TEST(FixedString, AssignTruncates)
{
  lvl::FixedString<8> s;
  EXPECT_TRUE(s.assign("short"));
  EXPECT_EQ(s, std::string_view{"short"});
  EXPECT_FALSE(s.assign("much too long"));
  EXPECT_EQ(s.size(), 7UL);
  EXPECT_EQ(s, std::string_view{"much to"});
}

TEST(Status, ErrorFormatsMessage)
{
  const lvl::Status ok = lvl::Status::okay();
  EXPECT_TRUE(ok.ok);
  const lvl::Status err = lvl::Status::error("cannot open '%s'", "x.json");
  EXPECT_FALSE(err.ok);
  EXPECT_EQ(err.message, std::string_view{"cannot open 'x.json'"});
}

TEST(Visitor, OverloadedDispatch)
{
  std::variant<int, float> v = 2.0F;
  const int which = std::visit(lvl::overloaded{[](int) { return 0; }, [](float) { return 1; }}, v);
  EXPECT_EQ(which, 1);
  static_assert(lvl::variant_index_v<float, std::variant<int, float>> == 1);
}
