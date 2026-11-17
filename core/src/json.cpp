/**
 * @copyright 2026-present Brian Cairl
 *
 * @file json.cpp
 */

// C++ Standard Library
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

// LVL
#include "lvl/core/file.hpp"
#include "lvl/core/json.hpp"
#include "lvl/core/logging.hpp"

namespace lvl::json
{
namespace
{

struct State
{
  memory::Arena* arena = nullptr;
  std::size_t depth = 0;
};

thread_local State state;

using InputAdapter =
  decltype(nlohmann::detail::input_adapter(std::declval<const char*>(), std::declval<const char*>()));

/// DOM-building SAX parser which records the parse error message instead of throwing
class ErrorCapturingParser : public nlohmann::detail::json_sax_dom_parser<Json, InputAdapter>
{
public:
  using Base = nlohmann::detail::json_sax_dom_parser<Json, InputAdapter>;

  ErrorCapturingParser(Json& root, FixedString<256>& message) : Base{root, false}, message_{&message} {}

  bool parse_error(std::size_t position, const std::string& last_token, const nlohmann::detail::exception& ex)
  {
    message_->assign(ex.what());
    return Base::parse_error(position, last_token, ex);
  }

private:
  FixedString<256>* message_;
};

}  // namespace

void install(memory::Arena& arena)
{
  LVL_ASSERT_NULL(state.arena) << "A JSON arena is already installed on this thread";
  LVL_ASSERT(memory::is_valid(arena));
  state = State{.arena = std::addressof(arena)};
}

void uninstall()
{
  LVL_ASSERT_NON_NULL(state.arena) << "No JSON arena is installed on this thread";
  LVL_ASSERT_EQ(state.depth, 0UL) << "A json::Scope is still open";
  state = State{};
}

bool is_installed() { return state.arena != nullptr; }

memory::Arena& active_arena()
{
  LVL_ASSERT_NON_NULL(state.arena) << "No JSON arena installed on this thread (see lvl::json::install)";
  LVL_ASSERT_GT(state.depth, 0UL) << "JSON allocation outside of a lvl::json::Scope";
  return *state.arena;
}

Scope::Scope()
{
  LVL_ASSERT_NON_NULL(state.arena) << "No JSON arena installed on this thread (see lvl::json::install)";
  mark_ = state.arena->utilized;
  depth_ = ++state.depth;
}

Scope::~Scope()
{
  LVL_ASSERT_EQ(state.depth, depth_) << "json::Scopes must end in the reverse order they began";
  state.arena->utilized = mark_;
  state.arena->last = mark_;
  --state.depth;
}

Status parse(std::string_view text, Json& out)
{
  FixedString<256> message;
  ErrorCapturingParser parser{out, message};
  // The parser is used directly (rather than via Json::sax_parse) so only the JSON text reader, not
  // the binary formats' readers, is instantiated for the arena-backed Json type.
  nlohmann::detail::parser<Json, InputAdapter> text_parser{
    nlohmann::detail::input_adapter(text.data(), text.data() + text.size()), nullptr, true, false};
  if (!text_parser.sax_parse(&parser, true) or parser.is_errored())
  {
    out = Json{};
    return Status::error("invalid JSON: %s", message.empty() ? "parse error" : message.c_str());
  }
  return Status::okay();
}

JsonString dump(const Json& j, int indent) { return j.dump(indent); }

int shortest(float v, char* buffer, std::size_t size)
{
  int n = 0;
  for (int precision = 6; precision <= 9; ++precision)
  {
    n = std::snprintf(buffer, size, "%.*g", precision, static_cast<double>(v));
    if (std::strtof(buffer, nullptr) == v)
    {
      break;
    }
  }
  return n;
}

double to_double(float v)
{
  char buffer[32];
  shortest(v, buffer, sizeof(buffer));
  return std::strtod(buffer, nullptr);
}

namespace detail
{

Status read_text(const char* path, std::string_view& text)
{
  char* data = nullptr;
  std::size_t size = 0;
  LVL_RETURN_IF_ERROR(file::read(path, active_arena(), data, size));
  text = std::string_view{data, size};
  return Status::okay();
}

Status write_text(const char* path, std::string_view text) { return file::write(path, text.data(), text.size()); }

}  // namespace detail
}  // namespace lvl::json
