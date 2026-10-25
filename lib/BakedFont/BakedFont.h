#pragma once

#include <cstddef>
#include <cstdint>

// Built-in reading faces. Each blob is YGZF (see XgfFont::loadPacked).
namespace BakedFont {

enum class Kind : uint8_t { Mincho, Gothic };

constexpr const char* kMinchoPath = "builtin:mincho";
constexpr const char* kGothicPath = "builtin:gothic";

bool bytes(Kind kind, const uint8_t*& data, size_t& size);

}  // namespace BakedFont
