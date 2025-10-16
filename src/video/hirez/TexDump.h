// SPDX-License-Identifier: GPL-3.0-or-later
// Part of melonDS – texture dump & replacement for the classic OpenGL backend.

#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <condition_variable>
#include <thread>
#include <deque>
#include <optional>
#include <functional>

// If you have stb available, set these to 1 to dump/load PNG.
// Otherwise, we’ll use our built-in tiny TGA writer/reader.
#ifndef TEXDUMP_WITH_STB
#define TEXDUMP_WITH_STB 0
#endif

namespace melonDS::hires {

enum class DsiTexFmt : uint8_t {
    Pal4   = 0,  // 2bpp indexed (4 colors)    – DS format 2
    Pal16  = 1,  // 4bpp indexed (16 colors)   – DS format 3
    Pal256 = 2,  // 8bpp indexed (256 colors)  – DS format 4
    Tex4x4 = 3,  // 4x4 texel compression      – DS format 5
    A5I3   = 4,  // 5-bit alpha, 3-bit index   – DS format 6
    A3I5   = 5,  // 3-bit alpha, 5-bit index   – DS format 1
    Direct = 6,  // 16-bit RGBA5551            – DS format 7
    Unknown= 15
};

struct TextureKey {
    uint64_t hash64;      // hash of decoded RGBA + invariants
    uint32_t width;
    uint32_t height;
    uint16_t flags;       // bit0: has_mips; bit1: color0_transparent; others reserved
    DsiTexFmt fmt;

    bool operator==(const TextureKey& o) const noexcept {
        return hash64==o.hash64 && width==o.width && height==o.height
            && flags==o.flags && fmt==o.fmt;
    }
};

struct TextureKeyHasher {
    size_t operator()(const TextureKey& k) const noexcept {
        // Simple mix for unordered_map/set
        auto x = k.hash64 ^ (uint64_t(k.width) << 32) ^ uint64_t(k.height);
        x ^= (uint64_t(k.flags) << 17) ^ (uint64_t(k.fmt) << 11);
        // 64->size_t
        return size_t(x ^ (x >> 33));
    }
};

struct TexDumpConfig {
    bool enableDump = false;
    bool enableReplace = false;
    // base directories (can be absolute or relative)
    std::filesystem::path dumpDir = "User/Dump/Textures";
    std::filesystem::path loadDir = "User/Load/Textures";
    // Dedup in-memory bloom/seen set size cap (number of entries)
    size_t inMemoryDedupBudget = 64'000;
    // Replacement image cache (compressed CPU RGBA) – bytes
    size_t replacementCacheBudgetBytes = 128ull * 1024ull * 1024ull;
    // Max pending I/O jobs
    size_t ioQueueCap = 4096;
    // File format preference
#if TEXDUMP_WITH_STB
    bool writePNG = true;  // PNG via stb_image_write
#else
    bool writePNG = false; // Fallback to TGA
#endif
};

// Initialize/shutdown once per emu instance (or when game changes).
void Init(const TexDumpConfig& cfg, const std::string& gameId);
void Shutdown();

// Update game id (e.g., when a new ROM is loaded)
void SetGameId(const std::string& gameId);

// Given decoded texture RGBA8, generate a key.
// The hasher includes width/height/flags/fmt and the RGBA contents.
TextureKey MakeKey(const uint8_t* rgba, uint32_t w, uint32_t h, bool hasMips,
                   bool pal0Transparent, DsiTexFmt fmt,
                   std::optional<uint64_t> paletteInvariantHash = std::nullopt);

// Enqueue a dump (non-blocking). Safe to call on GL/emu thread.
using PaletteIndexGenerator = std::function<bool(std::vector<uint8_t>&, std::string&, std::string&)>;

void DumpIfEnabled(const TextureKey& key, const uint8_t* rgba, uint32_t w, uint32_t h,
                   std::optional<uint64_t> paletteHash = std::nullopt,
                   const uint32_t* paletteRGBA = nullptr, uint32_t paletteCount = 0,
                   PaletteIndexGenerator paletteIndexGenerator = {});

// Try to synchronously load a replacement (CPU only). GL upload happens at call site.
// If found, returns true and fills rgbaOut (RGBA8) and outW/outH.
// Guaranteed non-blocking on GL thread except for filesystem stat/read; heavy decoding is avoided unless present.
bool TryLoadReplacement(const TextureKey& key, std::vector<uint8_t>& rgbaOut, uint32_t& outW, uint32_t& outH,
                        std::optional<uint64_t> paletteHash = std::nullopt,
                        std::string* usedFilename = nullptr);

// Utility helpers for naming.
std::string KeyToFilename(const TextureKey& key, bool pngExt);

// Optional: extract a DS-like 4-char game code from a ROM header (offset 0x0C).
// If romPath can't be opened, returns empty.
std::string ExtractNdsGameCodeFromRom(const std::filesystem::path& romPath);

// Query current enable flags (for fast-path guards in callers)
bool DumpEnabled();
bool ReplaceEnabled();

} // namespace melonDS::hires
