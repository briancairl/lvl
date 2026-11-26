// C++ Standard Library
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/core/binary.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/memory.hpp"

namespace test
{

enum class Shape
{
  kCircle,
  kSquare,
};

struct Thing
{
  lvl::Name name;
  int count = 0;
  float weight = 0.0F;
  bool enabled = false;
  Shape shape = Shape::kCircle;
  std::array<std::uint8_t, 3> color = {};
};

bool load(lvl::json::Reader& r, const lvl::Json& j, Thing& t)
{
  r.object(j, "thing");
  r.get(j, "name", t.name);
  r.get(j, "count", t.count);
  r.get(j, "weight", t.weight);
  r.get_optional(j, "enabled", t.enabled);
  r.get(j, "shape", t.shape);
  r.get(j, "color", t.color);
  return r.ok();
}

void save(lvl::Json& j, const Thing& t)
{
  j = lvl::Json::object();
  lvl::json::write(j, "name", t.name);
  lvl::json::write(j, "count", t.count);
  lvl::json::write(j, "weight", t.weight);
  lvl::json::write(j, "enabled", t.enabled);
  lvl::json::write(j, "shape", t.shape);
  lvl::json::write(j, "color", t.color);
}

template <typename S> void serialize(S& s, Thing& t)
{
  s.object(t.name);
  s.value4b(t.count);
  s.value4b(t.weight);
  s.value1b(t.enabled);
  s.value4b(t.shape);
  s.container1b(t.color);
}

/// The same thing as it was before a field was appended to it
struct OldThing
{
  lvl::Name name;
  int count = 0;
};

template <typename S> void serialize(S& s, OldThing& t)
{
  s.object(t.name);
  s.value4b(t.count);
}

/// OldThing with a field that only files of version 300 and up carry
struct GrownThing
{
  lvl::Name name;
  int count = 0;
  std::uint32_t extra = 0xFFFFFFFFU;
};

template <typename S> void serialize(S& s, GrownThing& t)
{
  s.object(t.name);
  s.value4b(t.count);
  if (lvl::binary::has_version(s, 300))
  {
    s.value4b(t.extra);
  }
  else if constexpr (std::is_same_v<S, lvl::binary::Reader>)
  {
    t.extra = 0;
  }
}

}  // namespace test

template <> struct lvl::EnumNames<test::Shape>
{
  static constexpr std::array<std::string_view, 2> kNames = {"circle", "square"};
};

class Serialization : public ::testing::Test
{
protected:
  void SetUp() override
  {
    lvl::memory::create(json_arena, 1 << 20, "json");
    lvl::memory::create(scratch, 1 << 20, "scratch");
    lvl::json::install(json_arena);
  }

  void TearDown() override
  {
    lvl::json::uninstall();
    lvl::memory::destroy(scratch);
    lvl::memory::destroy(json_arena);
  }

  static std::string temp(const char* name) { return std::string{::testing::TempDir()} + name; }

  lvl::memory::Arena json_arena;
  lvl::memory::Arena scratch;
};

TEST_F(Serialization, JsonRoundTripThroughFile)
{
  const test::Thing original{
    .name = "rock", .count = 3, .weight = 2.5F, .enabled = true, .shape = test::Shape::kSquare, .color = {1, 2, 3}};
  const auto path = temp("thing.json");
  ASSERT_TRUE(lvl::json::write_file(path.c_str(), [&](lvl::Json& j) { lvl::json::write(j, original); }).ok);
  EXPECT_EQ(json_arena.utilized, 0UL);

  test::Thing loaded;
  const auto status = lvl::json::read_file(path.c_str(), [&](const lvl::Json& j) {
    lvl::json::Reader r;
    r.convert(j, loaded, "");
    return r.status;
  });
  ASSERT_TRUE(status.ok) << status;
  EXPECT_EQ(loaded.name, original.name);
  EXPECT_EQ(loaded.count, 3);
  EXPECT_FLOAT_EQ(loaded.weight, 2.5F);
  EXPECT_TRUE(loaded.enabled);
  EXPECT_EQ(loaded.shape, test::Shape::kSquare);
  EXPECT_EQ(loaded.color, original.color);
  EXPECT_GT(json_arena.peak, 0UL);
}

TEST_F(Serialization, JsonReportsMalformedText)
{
  lvl::json::Scope scope;
  lvl::Json j;
  const auto status = lvl::json::parse("{\"a\": [1, 2,", j);
  EXPECT_FALSE(status.ok);
  EXPECT_NE(status.message.view().find("invalid JSON"), std::string_view::npos);
}

TEST_F(Serialization, JsonReaderReportsTypeErrors)
{
  lvl::json::Scope scope;
  lvl::Json j;
  ASSERT_TRUE(
    lvl::json::parse(R"({"name": "x", "count": "three", "weight": 1, "shape": "circle", "color": [0,0,0]})", j).ok);
  test::Thing t;
  lvl::json::Reader r;
  EXPECT_FALSE(r.convert(j, t, "thing"));
  EXPECT_NE(r.status.message.view().find("count"), std::string_view::npos) << r.status;
  EXPECT_NE(r.status.message.view().find("expected an integer"), std::string_view::npos) << r.status;
}

TEST_F(Serialization, JsonReaderChecksRangesAndEnums)
{
  lvl::json::Scope scope;
  lvl::Json j;
  ASSERT_TRUE(lvl::json::parse(R"({"small": 300, "shape": "triangle"})", j).ok);
  std::uint8_t small = 0;
  lvl::json::Reader r1;
  EXPECT_FALSE(r1.get(j, "small", small));
  test::Shape shape;
  lvl::json::Reader r2;
  EXPECT_FALSE(r2.get(j, "shape", shape));
  EXPECT_NE(r2.status.message.view().find("triangle"), std::string_view::npos);
}

TEST_F(Serialization, JsonMissingFileIsAnError)
{
  const auto status = lvl::json::read_file("/nonexistent/file.json", [](const lvl::Json&) { return lvl::Status{}; });
  EXPECT_FALSE(status.ok);
}

TEST_F(Serialization, BinaryRoundTripThroughFile)
{
  const test::Thing original{
    .name = "tree", .count = -7, .weight = 0.25F, .enabled = true, .shape = test::Shape::kSquare, .color = {9, 8, 7}};
  const auto path = temp("thing.bin");
  ASSERT_TRUE(lvl::binary::write_file(path.c_str(), scratch, lvl::binary::Kind::kTest, original).ok);
  EXPECT_EQ(scratch.utilized, 0UL);

  test::Thing loaded;
  const auto status = lvl::binary::read_file(path.c_str(), scratch, lvl::binary::Kind::kTest, loaded);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_EQ(loaded.name, original.name);
  EXPECT_EQ(loaded.count, -7);
  EXPECT_EQ(loaded.shape, test::Shape::kSquare);
  EXPECT_EQ(loaded.color, original.color);
}

TEST_F(Serialization, BinaryRejectsWrongKindAndTruncation)
{
  auto buffer = lvl::memory::make_vector<std::uint8_t>(scratch);
  const test::Thing original{.name = "x"};
  const std::size_t n = lvl::binary::serialize(buffer, lvl::binary::Kind::kTest, original);
  test::Thing loaded;
  EXPECT_FALSE(lvl::binary::deserialize(buffer.data(), n, lvl::binary::Kind::kLevel, loaded).ok);
  EXPECT_FALSE(lvl::binary::deserialize(buffer.data(), n - 2, lvl::binary::Kind::kTest, loaded).ok);
  EXPECT_TRUE(lvl::binary::deserialize(buffer.data(), n, lvl::binary::Kind::kTest, loaded).ok);
}

TEST_F(Serialization, BinaryReadsAFileWrittenBeforeAFieldExisted)
{
  // The version in the header is the (de)serializer's context, so a type can say which of its
  // fields a given file has. Writing is always the newest shape
  auto grown = lvl::memory::make_vector<std::uint8_t>(scratch);
  const test::GrownThing original{.name = "tree", .count = 5, .extra = 42};
  const std::size_t n = lvl::binary::serialize(grown, lvl::binary::Kind::kTest, original);
  test::GrownThing loaded;
  ASSERT_TRUE(lvl::binary::deserialize(grown.data(), n, lvl::binary::Kind::kTest, loaded).ok);
  EXPECT_EQ(loaded.extra, 42U);

  // The same bytes an editor two versions ago wrote: no "extra", and a header that says so
  auto old = lvl::memory::make_vector<std::uint8_t>(scratch);
  const test::OldThing before{.name = "tree", .count = 5};
  const std::size_t m = lvl::binary::serialize(old, lvl::binary::Kind::kTest, before);
  ASSERT_GE(m, 8UL);
  // The header is the 4-byte magic, then the version, little-endian
  const std::uint32_t older = 200;
  std::memcpy(old.data() + 4, &older, sizeof(older));
  test::GrownThing from_old{.extra = 7};
  const auto status = lvl::binary::deserialize(old.data(), m, lvl::binary::Kind::kTest, from_old);
  ASSERT_TRUE(status.ok) << status;
  EXPECT_EQ(from_old.name, std::string_view{"tree"});
  EXPECT_EQ(from_old.count, 5);
  EXPECT_EQ(from_old.extra, 0U);
  EXPECT_EQ(m + sizeof(std::uint32_t), n);
}

TEST(File, Extension)
{
  EXPECT_STREQ(lvl::file::extension("a/b.c/level.json"), ".json");
  EXPECT_STREQ(lvl::file::extension("a/b.c/level"), "");
}

TEST(JsonDeathTest, AllocationOutsideScopeAborts)
{
  lvl::memory::Arena arena;
  lvl::memory::create(arena, 1 << 16);
  lvl::json::install(arena);
  EXPECT_DEATH({ lvl::Json j = lvl::Json::array({1, 2, 3}); }, "outside of a lvl::json::Scope");
  lvl::json::uninstall();
  lvl::memory::destroy(arena);
}
