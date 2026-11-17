// GTest
#include <gtest/gtest.h>

// LVL
#include "lvl/editor/undo.hpp"

using namespace lvl;
using namespace lvl::level;

class Undo : public ::testing::Test
{
protected:
  void SetUp() override
  {
    memory::create(doc, 8 << 20, "doc");
    memory::create(ring, 4096, "undo");
    level = new (storage) Level{doc};
    create_default(*level, Settings{.extents = {16, 16}});
    undo.initialize(ring);
  }
  void TearDown() override
  {
    level->~Level();
    memory::destroy(ring);
    memory::destroy(doc);
  }

  TileGrid& ground() { return plane<TileGrid>(level->layers[1]); }

  memory::Arena doc;
  memory::Arena ring;
  alignas(Level) unsigned char storage[sizeof(Level)];
  Level* level = nullptr;
  editor::UndoStack undo;
};

TEST_F(Undo, UndoRedoSwapsSnapshots)
{
  undo.snapshot(*level, 1, "paint a");
  ground().at(1, 1) = TileRef{0, 1};
  undo.snapshot(*level, 1, "paint b");
  ground().at(2, 2) = TileRef{0, 2};
  EXPECT_STREQ(undo.undo_label(), "paint b");

  ASSERT_TRUE(undo.undo(*level));
  EXPECT_TRUE(ground().at(2, 2).empty());
  EXPECT_EQ(ground().at(1, 1), (TileRef{0, 1}));
  ASSERT_TRUE(undo.undo(*level));
  EXPECT_TRUE(ground().at(1, 1).empty());
  EXPECT_FALSE(undo.can_undo());

  ASSERT_TRUE(undo.redo(*level));
  EXPECT_EQ(ground().at(1, 1), (TileRef{0, 1}));
  ASSERT_TRUE(undo.redo(*level));
  EXPECT_EQ(ground().at(2, 2), (TileRef{0, 2}));
  EXPECT_FALSE(undo.can_redo());
}

TEST_F(Undo, NewEditClearsRedo)
{
  undo.snapshot(*level, 1, "a");
  ground().at(0, 0) = TileRef{0, 1};
  undo.undo(*level);
  EXPECT_TRUE(undo.can_redo());
  undo.snapshot(*level, 1, "b");
  EXPECT_FALSE(undo.can_redo());
}

TEST_F(Undo, GroupsUndoTogether)
{
  undo.begin_group();
  undo.snapshot(*level, 0, "gen");
  undo.snapshot(*level, 1, "gen");
  undo.end_group();
  plane<ScalarGrid>(level->layers[0]).at(3, 3) = 5.0F;
  ground().at(3, 3) = TileRef{0, 3};
  EXPECT_EQ(undo.size(), 2UL);
  ASSERT_TRUE(undo.undo(*level));
  EXPECT_FALSE(undo.can_undo());
  EXPECT_EQ(plane<ScalarGrid>(level->layers[0]).at(3, 3), 0.0F);
  EXPECT_TRUE(ground().at(3, 3).empty());
}

TEST_F(Undo, RingEvictsOldest)
{
  // A 16x16 tile layer is 1 KiB; the ring holds 4 KiB
  for (int i = 0; i < 10; ++i)
  {
    undo.snapshot(*level, 1, "edit");
    ground().at(i, 0) = TileRef{0, static_cast<TileIndex>(i)};
  }
  EXPECT_LE(undo.size(), 4UL);
  EXPECT_GE(undo.size(), 3UL);
  while (undo.undo(*level))
  {
  }
  // The oldest edits can no longer be undone, the newest were
  EXPECT_EQ(ground().at(0, 0), (TileRef{0, 0}));
  EXPECT_TRUE(ground().at(9, 0).empty());
}

TEST_F(Undo, StaleHistoryIsDropped)
{
  undo.snapshot(*level, 1, "a");
  resize(*level, Extents{8, 8});
  EXPECT_FALSE(undo.undo(*level));
  EXPECT_FALSE(undo.can_undo());
}
