/**
 * @copyright 2026-present Brian Cairl
 *
 * @file undo.hpp
 *
 * Undo/redo of layer edits, by snapshot: before an edit (a brush stroke, a fill, a generator run),
 * the affected layers' planes are copied into a ring buffer in the undo arena. Undo swaps the
 * snapshot with the layer's current cells, turning the entry into its own redo (and vice versa), so
 * no separate redo copies are kept. When the ring is full the oldest entries are dropped.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstddef>
#include <cstdint>

// LVL
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/level/level.hpp"

namespace lvl::editor
{

class UndoStack
{
public:
  static constexpr std::size_t kMaxEntries = 512;

  /// Uses <code>arena</code> (all of it) as the snapshot ring
  void initialize(memory::Arena& arena);

  /// Forgets all history (e.g. on new/load)
  void clear();

  /**
   * @brief Snapshots layer <code>layer</code> as part of the edit labelled <code>label</code>
   *
   * Snapshots taken between <code>begin_group</code>/<code>end_group</code> are undone together.
   * Clears the redo history.
   */
  void snapshot(const level::Level& level, std::size_t layer, const char* label);

  /// Groups the following snapshots into one undo step
  void begin_group();
  void end_group();

  [[nodiscard]] bool can_undo() const { return undo_count_ > 0; }
  [[nodiscard]] bool can_redo() const { return redo_count_ > 0; }

  /// Label of the step <code>undo()</code>/<code>redo()</code> would apply
  [[nodiscard]] const char* undo_label() const;
  [[nodiscard]] const char* redo_label() const;

  /// Reverts the most recent step; returns false if there was nothing (or it no longer applies)
  bool undo(level::Level& level);

  /// Re-applies the most recently undone step
  bool redo(level::Level& level);

  [[nodiscard]] std::size_t size() const { return undo_count_; }

private:
  struct Entry
  {
    std::size_t layer = 0;
    level::LayerKind kind = level::LayerKind::kTiles;
    level::Extents extents;
    std::size_t offset = 0;
    std::size_t bytes = 0;
    std::uint32_t group = 0;
    FixedString<48> label;
  };

  /// Reserves <code>bytes</code> in the ring (evicting the oldest entries as needed)
  std::uint8_t* reserve(std::size_t bytes, std::size_t& offset);

  /// Swaps an entry's snapshot with its layer's current cells
  bool swap(level::Level& level, const Entry& entry);

  std::uint8_t* ring_ = nullptr;
  std::size_t capacity_ = 0;
  /// Entries: [0, undo_count_) undo stack (oldest first), then redo entries above (newest redo last)
  std::array<Entry, kMaxEntries> entries_;
  std::size_t first_ = 0;
  std::size_t undo_count_ = 0;
  std::size_t redo_count_ = 0;
  std::uint32_t next_group_ = 1;
  std::uint32_t open_group_ = 0;
  int group_depth_ = 0;
};

}  // namespace lvl::editor
