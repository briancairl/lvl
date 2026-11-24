/**
 * @copyright 2026-present Brian Cairl
 *
 * @file binary.hpp
 *
 * Compact binary (de)serialization with Bitsery. Bitsery allocates nothing itself: output goes to an
 * arena-backed byte vector and input is read in place from a buffer the file was loaded into (also
 * an arena), so binary save/load memory is bounded by the arena it is given.
 *
 * Every binary file starts with a Header naming its kind and the editor version which wrote it.
 * Binary files are always a translation of the JSON form of the same data (see CLAUDE.md), and
 * types implement both.
 */
#pragma once

// C++ Standard Library
#include <array>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Bitsery
#include <bitsery/adapter/buffer.h>
#include <bitsery/bitsery.h>
#include <bitsery/traits/array.h>
#include <bitsery/traits/vector.h>

// LVL
#include "lvl/core/build.hpp"
#include "lvl/core/file.hpp"
#include "lvl/core/fixed_string.hpp"
#include "lvl/core/memory.hpp"
#include "lvl/core/status.hpp"

namespace lvl::binary
{

/// Arena-backed output buffer
using Buffer = memory::Vector<std::uint8_t>;

using OutputAdapter = bitsery::OutputBufferAdapter<Buffer>;
using InputAdapter = bitsery::InputBufferAdapter<const std::uint8_t*>;

/// Serializer writing into a Buffer
using Writer = bitsery::Serializer<OutputAdapter>;

/// Deserializer reading in place from a byte range
using Reader = bitsery::Deserializer<InputAdapter>;

/// "LVLB"
inline constexpr std::array<char, 4> kMagic = {'L', 'V', 'L', 'B'};

/// What a binary file contains
enum class Kind : std::uint32_t
{
  kLevel = 1,
  kTileSet = 2,
  kWfcRules = 3,
  kPathGraph = 4,
  /// A level whose shared data (catalogs, seasons, tilesets, rule sets) lives in its project
  kProjectLevel = 5,
  kTest = 0xFFFF,
};

struct Header
{
  std::array<char, 4> magic = kMagic;
  std::uint32_t version = static_cast<std::uint32_t>(kVersion);
  Kind kind = Kind::kTest;
};

template <typename S> void serialize(S& s, Header& h)
{
  s.container1b(h.magic);
  s.value4b(h.version);
  s.value4b(h.kind);
}

/**
 * @brief Serializes <code>value</code> (preceded by a Header of <code>kind</code>) into <code>buffer</code>
 *
 * <code>value</code> needs an ADL <code>serialize(S&, T&)</code>. Returns the number of bytes written.
 */
template <typename T> std::size_t serialize(Buffer& buffer, Kind kind, const T& value)
{
  Writer writer{buffer};
  Header header{.kind = kind};
  writer.object(header);
  writer.object(const_cast<T&>(value));
  writer.adapter().flush();
  const std::size_t written = writer.adapter().writtenBytesCount();
  buffer.resize(written);
  return written;
}

/// Deserializes a Header of <code>kind</code> followed by <code>value</code> from <code>[data, data + size)</code>
template <typename T> Status deserialize(const std::uint8_t* data, std::size_t size, Kind kind, T& value)
{
  Reader reader{InputAdapter{data, size}};
  Header header;
  reader.object(header);
  if (reader.adapter().error() != bitsery::ReaderError::NoError or header.magic != kMagic)
  {
    return Status::error("not a lvl binary file");
  }
  if (header.kind != kind)
  {
    return Status::error(
      "binary file holds kind %u, expected %u", static_cast<unsigned>(header.kind), static_cast<unsigned>(kind));
  }
  if (header.version > static_cast<std::uint32_t>(kVersion))
  {
    return Status::error("binary file written by a newer editor (version %u)", static_cast<unsigned>(header.version));
  }
  reader.object(value);
  switch (reader.adapter().error())
  {
  case bitsery::ReaderError::NoError:
    break;
  case bitsery::ReaderError::DataOverflow:
    return Status::error("binary data truncated");
  case bitsery::ReaderError::InvalidData:
    return Status::error("binary data invalid");
  case bitsery::ReaderError::InvalidPointer:
    return Status::error("binary data has an invalid pointer");
  case bitsery::ReaderError::ReadingError:
    return Status::error("binary read error");
  }
  if (!reader.adapter().isCompletedSuccessfully())
  {
    return Status::error("trailing bytes after binary data");
  }
  return Status::okay();
}

/// Serializes <code>value</code> into <code>scratch</code> (rewound afterwards) and writes it to <code>path</code>
template <typename T> Status write_file(const char* path, memory::Arena& scratch, Kind kind, const T& value)
{
  memory::Scope scope{scratch};
  auto buffer = memory::make_vector<std::uint8_t>(scratch);
  const std::size_t n = serialize(buffer, kind, value);
  return file::write(path, buffer.data(), n);
}

/// Reads <code>path</code> into <code>scratch</code> (rewound afterwards) and deserializes it into <code>value</code>
///
/// <code>value</code> must not allocate from <code>scratch</code> while deserializing
template <typename T> Status read_file(const char* path, memory::Arena& scratch, Kind kind, T& value)
{
  memory::Scope scope{scratch};
  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, scratch, data, size));
  if (Status s = deserialize(reinterpret_cast<const std::uint8_t*>(data), size, kind, value); !s.ok)
  {
    return Status::error("%s: %s", path, s.message.c_str());
  }
  return Status::okay();
}

}  // namespace lvl::binary

namespace lvl
{

/// Bitsery serialization of FixedString: a 2-byte length followed by the characters
template <typename S, std::size_t N> void serialize(S& s, FixedString<N>& str)
{
  static_assert(N <= 0xFFFF);
  std::uint16_t length = static_cast<std::uint16_t>(str.size());
  s.value2b(length);
  if constexpr (std::is_same_v<S, binary::Reader>)
  {
    str.clear();
    if (length > FixedString<N>::kMaxLength)
    {
      s.adapter().error(bitsery::ReaderError::InvalidData);
      return;
    }
  }
  for (std::uint16_t i = 0; i < length; ++i)
  {
    s.value1b(str.data[i]);
  }
}

}  // namespace lvl
