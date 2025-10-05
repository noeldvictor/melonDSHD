// SPDX-License-Identifier: GPL-3.0-or-later
// Simple sprite dumper for 2D renderer (OBJ). Dump only; no replacement.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace melonDS::sprites {

struct SpriteDumpConfig {
    bool enableDump = false;
    bool enableReplace = true; // allow replacement lookups (call sites can gate usage)
    bool swapRB = false; // optional: swap R/B when converting replacements
    std::filesystem::path dumpDir = "User/Dump/Sprites";
    std::filesystem::path loadDir = "User/Load/Sprites";
#ifndef SPRITEDUMP_WITH_STB
#define SPRITEDUMP_WITH_STB 0
#endif
#if SPRITEDUMP_WITH_STB
    bool writePNG = true;
#else
    bool writePNG = false; // fallback to TGA
#endif
};

enum class ObjFmt : uint8_t { Pal16=0, Pal256=1, Bitmap=2, Unknown=15 };

struct SpriteKey {
    uint64_t hash64{}; uint32_t width{}, height{}; ObjFmt fmt{ObjFmt::Unknown};
};

void Init(const SpriteDumpConfig& cfg, const std::string& gameId);
void Shutdown();

SpriteKey MakeKey(const uint8_t* rgba, uint32_t w, uint32_t h, ObjFmt fmt);
void DumpIfEnabled(const SpriteKey& key, const uint8_t* rgba, uint32_t w, uint32_t h);
std::string KeyToFilename(const SpriteKey& key, bool pngExt);

// Try to load a replacement image (RGBA8). Returns true on success.
// The output size may be an integer multiple of the original sprite size.
bool TryLoadReplacement(const SpriteKey& key, std::vector<uint8_t>& rgbaOut, uint32_t& outW, uint32_t& outH);

bool DumpEnabled();
bool ReplaceEnabled();
bool SwapRBEnabled();

} // namespace melonDS::sprites
