/**
 * @copyright 2026-present Brian Cairl
 *
 * @file undo.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cstring>

// LVL
#include "lvl/core/logging.hpp"
#include "lvl/editor/undo.hpp"

namespace lvl::editor
{
namespace
{

std::size_t plane_bytes(const level::Layer& layer)
{
  return std::visit(
    [](const auto& g) { return g.values.size() * sizeof(typename std::decay_t<decltype(g)>::value_type); }, layer.data);
}

void* plane_data(level::Layer& layer)
{
  return std::visit([](auto& g) { return static_cast<void*>(g.values.data()); }, layer.data);
}

const void* plane_data(const level::Layer& layer)
{
  return std::visit([](const auto& g) { return static_cast<const void*>(g.values.data()); }, layer.data);
}

}  // namespace

void UndoStack::initialize(memory::Arena& arena)
{
  capacity_ = memory::available(arena);
  ring_ = memory::allocate<std::uint8_t>(arena, capacity_);
  clear();
}

void UndoStack::clear()
{
  first_ = 0;
  undo_count_ = 0;
  redo_count_ = 0;
  open_group_ = 0;
  group_depth_ = 0;
}

const char* UndoStack::undo_label() const
{
  return can_undo() ? entries_[(first_ + undo_count_ - 1) % kMaxEntries].label.c_str() : "";
}

const char* UndoStack::redo_label() const
{
  return can_redo() ? entries_[(first_ + undo_count_) % kMaxEntries].label.c_str() : "";
}

void UndoStack::begin_group()
{
  if (group_depth_++ == 0)
  {
    open_group_ = next_group_++;
  }
}

void UndoStack::end_group()
{
  LVL_ASSERT_GT(group_depth_, 0) << "unbalanced UndoStack::end_group";
  if (--group_depth_ == 0)
  {
    open_group_ = 0;
  }
}

std::uint8_t* UndoStack::reserve(std::size_t bytes, std::size_t& offset)
{
  if (bytes > capacity_)
  {
    return nullptr;
  }
  while (true)
  {
    if (undo_count_ == 0)
    {
      first_ = 0;
      offset = 0;
      return ring_;
    }
    const Entry& oldest = entries_[first_];
    const Entry& newest = entries_[(first_ + undo_count_ - 1) % kMaxEntries];
    const std::size_t head = newest.offset + newest.bytes;
    if (undo_count_ < kMaxEntries)
    {
      if (head > oldest.offset or (head == oldest.offset and undo_count_ == 0))
      {
        // Free: [head, capacity) and [0, oldest.offset)
        if (capacity_ - head >= bytes)
        {
          offset = head;
          return ring_ + offset;
        }
        if (oldest.offset >= bytes)
        {
          offset = 0;
          return ring_;
        }
      }
      else if (head < oldest.offset and oldest.offset - head >= bytes)
      {
        offset = head;
        return ring_ + offset;
      }
    }
    // Evict the oldest whole step
    const std::uint32_t group = oldest.group;
    do
    {
      first_ = (first_ + 1) % kMaxEntries;
      --undo_count_;
    } while (undo_count_ > 0 and group != 0 and entries_[first_].group == group);
  }
}

void UndoStack::snapshot(const level::Level& level, std::size_t layer_index, const char* label)
{
  LVL_ASSERT_LT(layer_index, level.layers.size());
  const level::Layer& layer = level.layers[layer_index];
  redo_count_ = 0;
  const std::size_t bytes = plane_bytes(layer);
  std::size_t offset = 0;
  std::uint8_t* const dst = reserve(bytes, offset);
  if (dst == nullptr)
  {
    LVL_LOG_WARN() << "Layer '" << layer.name << "' is too large to undo (" << memory::Bytes{bytes} << ")";
    return;
  }
  std::memcpy(dst, plane_data(layer), bytes);
  Entry& e = entries_[(first_ + undo_count_) % kMaxEntries];
  e = Entry{
    .layer = layer_index,
    .kind = layer.kind,
    .extents = level::extents(layer),
    .offset = offset,
    .bytes = bytes,
    .group = (open_group_ != 0) ? open_group_ : next_group_++,
    .label = {}};
  e.label.assign(label);
  ++undo_count_;
}

bool UndoStack::swap(level::Level& level, const Entry& e)
{
  if (e.layer >= level.layers.size())
  {
    return false;
  }
  level::Layer& layer = level.layers[e.layer];
  if (layer.kind != e.kind or level::extents(layer) != e.extents)
  {
    // The layer was replaced/resized since: the snapshot no longer applies
    return false;
  }
  auto* const cells = static_cast<std::uint8_t*>(plane_data(layer));
  std::uint8_t* const saved = ring_ + e.offset;
  // Swap in chunks through a small stack buffer
  std::uint8_t tmp[4096];
  for (std::size_t done = 0; done < e.bytes; done += sizeof(tmp))
  {
    const std::size_t n = std::min(sizeof(tmp), e.bytes - done);
    std::memcpy(tmp, cells + done, n);
    std::memcpy(cells + done, saved + done, n);
    std::memcpy(saved + done, tmp, n);
  }
  return true;
}

bool UndoStack::undo(level::Level& level)
{
  if (!can_undo())
  {
    return false;
  }
  const std::uint32_t group = entries_[(first_ + undo_count_ - 1) % kMaxEntries].group;
  bool ok = true;
  while (undo_count_ > 0 and entries_[(first_ + undo_count_ - 1) % kMaxEntries].group == group)
  {
    ok = swap(level, entries_[(first_ + undo_count_ - 1) % kMaxEntries]) and ok;
    --undo_count_;
    ++redo_count_;
  }
  if (!ok)
  {
    LVL_LOG_WARN() << "Undo history no longer matches the layers; clearing it";
    clear();
  }
  return ok;
}

bool UndoStack::redo(level::Level& level)
{
  if (!can_redo())
  {
    return false;
  }
  const std::uint32_t group = entries_[(first_ + undo_count_) % kMaxEntries].group;
  bool ok = true;
  while (redo_count_ > 0 and entries_[(first_ + undo_count_) % kMaxEntries].group == group)
  {
    ok = swap(level, entries_[(first_ + undo_count_) % kMaxEntries]) and ok;
    ++undo_count_;
    --redo_count_;
  }
  if (!ok)
  {
    LVL_LOG_WARN() << "Redo history no longer matches the layers; clearing it";
    clear();
  }
  return ok;
}

}  // namespace lvl::editor
