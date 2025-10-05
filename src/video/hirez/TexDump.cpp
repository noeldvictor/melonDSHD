// SPDX-License-Identifier: GPL-3.0-or-later
#include "TexDump.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <chrono>
#include <algorithm>

#if TEXDUMP_WITH_STB
  // Provide your local path to stb if needed; these are single-header public domain libraries.
  #define STB_IMAGE_WRITE_IMPLEMENTATION
  #include "thirdparty/stb/stb_image_write.h"
  #define STB_IMAGE_IMPLEMENTATION
  #include "thirdparty/stb/stb_image.h"
#endif

namespace fs = std::filesystem;
namespace melonDS::hires {

// ----------- Small utils (FNV-1a 64) -----------------
static inline uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
static inline std::string to_hex(uint64_t x) {
    static const char* d = "0123456789abcdef";
    std::string s(16,'0');
    for (int i = 15; i >= 0; --i) { s[i] = d[x & 0xF]; x >>= 4; }
    return s;
}
static inline const char* fmt_name(DsiTexFmt f) {
    switch (f) {
        case DsiTexFmt::Pal4: return "pal4";
        case DsiTexFmt::Pal16: return "pal16";
        case DsiTexFmt::Pal256: return "pal256";
        case DsiTexFmt::Tex4x4: return "tex4x4";
        case DsiTexFmt::A5I3: return "a5i3";
        case DsiTexFmt::A3I5: return "a3i5";
        case DsiTexFmt::Direct: return "rgba5551";
        default: return "unk";
    }
}
// Minimal TGA writer/reader (uncompressed BGRA32), trivial and fast.
static bool write_tga(const fs::path& p, const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> buf;
    buf.reserve(18 + size_t(w)*h*4);
    uint8_t hdr[18] = {};
    hdr[2] = 2; // uncompressed true-color
    hdr[12] = uint8_t(w & 0xFF);
    hdr[13] = uint8_t((w >> 8) & 0xFF);
    hdr[14] = uint8_t(h & 0xFF);
    hdr[15] = uint8_t((h >> 8) & 0xFF);
    hdr[16] = 32; // bpp
    hdr[17] = 8;  // alpha bits
    buf.insert(buf.end(), hdr, hdr + 18);
    // TGA expects BGRA, bottom-to-top by default; set origin to top-left by flipping bit 5
    buf[17] |= 0x20;
    // Convert RGBA->BGRA, keep row order (top-down)
    const size_t N = size_t(w)*h;
    buf.resize(18 + N*4);
    uint8_t* out = buf.data() + 18;
    for (size_t i = 0; i < N; ++i) {
        out[i*4+0] = rgba[i*4+2];
        out[i*4+1] = rgba[i*4+1];
        out[i*4+2] = rgba[i*4+0];
        out[i*4+3] = rgba[i*4+3];
    }
    std::error_code ec; fs::create_directories(p.parent_path(), ec);
    FILE* f = std::fopen(p.string().c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return ok;
}
static bool read_tga(const fs::path& p, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    uint8_t hdr[18]{};
    f.read(reinterpret_cast<char*>(hdr), 18);
    if (!f) return false;
    const uint8_t idlen = hdr[0];
    const uint8_t ctype = hdr[2]; // 2=uncompressed true-color, 10=RLE true-color
    const uint16_t cmaplen = uint16_t(hdr[5]) | (uint16_t(hdr[6]) << 8);
    const uint8_t bpp = hdr[16];
    const bool topLeft = (hdr[17] & 0x20) != 0;
    if (!(ctype == 2 || ctype == 10)) return false;
    if (!(bpp == 24 || bpp == 32)) return false;
    w = uint32_t(hdr[12]) | (uint32_t(hdr[13]) << 8);
    h = uint32_t(hdr[14]) | (uint32_t(hdr[15]) << 8);

    // Skip ID field and color map if present
    if (idlen) f.seekg(idlen, std::ios::cur);
    if (hdr[1] != 0 && cmaplen) {
        uint16_t cmap_entry_size = hdr[7];
        size_t cmap_bytes = (size_t(cmaplen) * cmap_entry_size + 7) / 8;
        f.seekg(cmap_bytes, std::ios::cur);
    }

    rgba.assign(size_t(w) * h * 4, 0);
    auto put_pixel = [&](uint32_t x, uint32_t y, uint8_t B, uint8_t G, uint8_t R, uint8_t A){
        if (!topLeft) y = (h - 1 - y);
        size_t idx = (size_t(y) * w + x) * 4;
        rgba[idx+0] = R;
        rgba[idx+1] = G;
        rgba[idx+2] = B;
        rgba[idx+3] = A;
    };

    if (ctype == 2) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t b=0,g=0,r=0,a=255;
                b = uint8_t(f.get());
                g = uint8_t(f.get());
                r = uint8_t(f.get());
                if (bpp == 32) a = uint8_t(f.get());
                if (!f) return false;
                put_pixel(x, y, b, g, r, a);
            }
        }
        return true;
    }
    // RLE decoding
    uint32_t x = 0, y = 0;
    while (y < h) {
        uint8_t packet = uint8_t(f.get());
        if (!f) return false;
        uint8_t count = (packet & 0x7F) + 1;
        if (packet & 0x80) {
            // RLE packet
            uint8_t b=0,g=0,r=0,a=255;
            b = uint8_t(f.get());
            g = uint8_t(f.get());
            r = uint8_t(f.get());
            if (bpp == 32) a = uint8_t(f.get());
            if (!f) return false;
            for (uint8_t i = 0; i < count; ++i) {
                put_pixel(x, y, b, g, r, a);
                if (++x >= w) { x = 0; if (++y >= h) break; }
            }
        } else {
            // Raw packet
            for (uint8_t i = 0; i < count; ++i) {
                uint8_t b=0,g=0,r=0,a=255;
                b = uint8_t(f.get());
                g = uint8_t(f.get());
                r = uint8_t(f.get());
                if (bpp == 32) a = uint8_t(f.get());
                if (!f) return false;
                put_pixel(x, y, b, g, r, a);
                if (++x >= w) { x = 0; if (++y >= h) break; }
            }
        }
    }
    return true;
}

// -------------- globals --------------
static TexDumpConfig G;
static std::string GGameId;
static std::atomic<bool> GRunning{false};
static bool GVerbose = []{
    const char* v = std::getenv("MELONDS_TEX_VERBOSE");
    return v && *v && *v != '0';
}();

struct DumpJob {
    fs::path path;
    std::vector<uint8_t> rgba;
    uint32_t w, h;
    bool png;
};
static std::mutex Qmtx;
static std::condition_variable Qcv;
static std::deque<DumpJob> Q;
static std::thread Qthread;

static std::mutex SeenMtx;
static std::unordered_set<std::string> Seen; // filenames we already dumped this session (kept ≤ budget)

struct CacheEntry { std::vector<uint8_t> rgba; uint32_t w=0, h=0; size_t size() const { return rgba.size(); } };
static std::mutex CacheMtx;
static std::unordered_map<std::string, CacheEntry> Cache;
static size_t CacheBytes = 0;

// -------------- worker --------------
static void worker() {
    while (GRunning.load(std::memory_order_acquire)) {
        DumpJob job;
        {
            std::unique_lock<std::mutex> lk(Qmtx);
            Qcv.wait(lk, []{ return !GRunning.load(std::memory_order_acquire) || !Q.empty(); });
            if (!GRunning.load(std::memory_order_acquire) && Q.empty()) break;
            job = std::move(Q.front());
            Q.pop_front();
        }
        // Skip if exists
        std::error_code ec;
        if (fs::exists(job.path, ec)) continue;
#if TEXDUMP_WITH_STB
        if (job.png) {
            fs::create_directories(job.path.parent_path(), ec);
            stbi_write_png(job.path.string().c_str(), job.w, job.h, 4, job.rgba.data(), int(job.w*4));
        } else
#endif
        {
            write_tga(job.path, job.rgba.data(), job.w, job.h);
        }
    }
}

// -------------- API --------------
void Init(const TexDumpConfig& cfg, const std::string& gameId) {
    Shutdown();
    G = cfg;
    SetGameId(gameId);
    Seen.clear(); Seen.reserve(G.inMemoryDedupBudget);
    Cache.clear(); CacheBytes = 0;
    if (G.enableDump) {
        GRunning.store(true, std::memory_order_release);
        Qthread = std::thread(worker);
    }
}
void Shutdown() {
    if (GRunning.exchange(false, std::memory_order_acq_rel)) {
        Qcv.notify_all();
        if (Qthread.joinable()) Qthread.join();
    }
    {
        std::lock_guard<std::mutex> lk(Qmtx);
        Q.clear();
    }
    {
        std::lock_guard<std::mutex> lk(CacheMtx);
        Cache.clear(); CacheBytes = 0;
    }
    {
        std::lock_guard<std::mutex> lk(SeenMtx);
        Seen.clear();
    }
}
void SetGameId(const std::string& gameId) { GGameId = gameId; }

TextureKey MakeKey(const uint8_t* rgba, uint32_t w, uint32_t h, bool hasMips, bool pal0Transparent, DsiTexFmt fmt) {
    // Hash the contents; include invariants to avoid cross-format collisions
    uint64_t h1 = fnv1a64(rgba, size_t(w)*h*4, 0xcbf29ce484222325ull);
    uint64_t h2 = fnv1a64(&w, sizeof(w), h1);
    h2 = fnv1a64(&h, sizeof(h), h2);
    uint16_t flags = (hasMips?1:0) | (pal0Transparent?2:0);
    h2 = fnv1a64(&flags, sizeof(flags), h2);
    uint8_t F = (uint8_t)fmt;
    h2 = fnv1a64(&F, sizeof(F), h2);
    return TextureKey{ h2, w, h, flags, fmt };
}

std::string KeyToFilename(const TextureKey& key, bool pngExt) {
    // Dolphin-like: tex1_<WxH>[_m]_<hash>_<fmt>.<ext>
    std::string name = "tex1_" + std::to_string(key.width) + "x" + std::to_string(key.height);
    if (key.flags & 1) name += "_m";
    name += "_" + to_hex(key.hash64) + "_" + std::string(fmt_name(key.fmt));
    name += pngExt ? ".png" : ".tga";
    return name;
}

static fs::path GameDumpDir() { return G.dumpDir / (GGameId.empty() ? fs::path("Unknown") : fs::path(GGameId)); }
static fs::path GameLoadDir() { return G.loadDir / (GGameId.empty() ? fs::path("Unknown") : fs::path(GGameId)); }

void DumpIfEnabled(const TextureKey& key, const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!G.enableDump) return;
    // Build filename
    const bool png = G.writePNG;
    fs::path dst = GameDumpDir() / KeyToFilename(key, png);
    // Dedup in memory first
    {
        std::lock_guard<std::mutex> lk(SeenMtx);
        if (Seen.find(dst.string()) != Seen.end()) return;
        // If file exists on disk, remember it and skip.
        std::error_code ec;
        if (fs::exists(dst, ec)) { Seen.insert(dst.string()); return; }
        if (Seen.size() >= G.inMemoryDedupBudget) {
            // Simple pruning: erase half (not LRU, but cheap)
            size_t n = G.inMemoryDedupBudget / 2;
            auto it = Seen.begin();
            for (size_t i=0; i<n && it!=Seen.end(); ++i) it = Seen.erase(it);
        }
        Seen.insert(dst.string());
    }
    // Enqueue
    std::vector<uint8_t> copy(rgba, rgba + size_t(w)*h*4);
    {
        std::lock_guard<std::mutex> lk(Qmtx);
        if (Q.size() >= G.ioQueueCap) return; // backpressure: drop
        Q.push_back(DumpJob{ std::move(dst), std::move(copy), w, h, png });
    }
    Qcv.notify_one();
}

bool TryLoadReplacement(const TextureKey& key, std::vector<uint8_t>& rgbaOut, uint32_t& outW, uint32_t& outH) {
    if (!G.enableReplace) return false;
    const bool png = G.writePNG; // if we write PNG we’ll also try reading PNG first
    fs::path base = GameLoadDir();
    fs::path p1 = base / KeyToFilename(key, true);
    fs::path p2 = base / KeyToFilename(key, false);

    auto tryFile = [&](const fs::path& p)->bool {
        // Cache by absolute filename
        const std::string k = p.string();
        {   // cache hit
            std::lock_guard<std::mutex> lk(CacheMtx);
            auto it = Cache.find(k);
            if (it != Cache.end()) {
                if (GVerbose) std::fprintf(stderr, "[tex] cache hit: %s (%ux%u)\n", k.c_str(), it->second.w, it->second.h);
                rgbaOut = it->second.rgba; outW = it->second.w; outH = it->second.h;
                return true;
            }
        }
        // load
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            if (GVerbose) std::fprintf(stderr, "[tex] not found: %s\n", k.c_str());
            return false;
        }

        uint32_t w=0,h=0; std::vector<uint8_t> tmp;
#if TEXDUMP_WITH_STB
        if (p.extension() == ".png") {
            int W,H,Comp;
            unsigned char* data = stbi_load(p.string().c_str(), &W, &H, &Comp, 4);
            if (!data) return false;
            tmp.assign(data, data + size_t(W)*H*4);
            stbi_image_free(data);
            w = uint32_t(W); h = uint32_t(H);
        } else
#endif
        {
            if (!read_tga(p, tmp, w, h)) {
                if (GVerbose) std::fprintf(stderr, "[tex] tga load failed: %s\n", k.c_str());
                return false;
            }
        }
        // Insert to cache with LRU-bytes cap
        {
            std::lock_guard<std::mutex> lk(CacheMtx);
            const size_t add = tmp.size();
            while (CacheBytes + add > G.replacementCacheBudgetBytes && !Cache.empty()) {
                // Arbitrary eviction (not precise LRU to keep it tiny)
                auto it = Cache.begin();
                CacheBytes -= it->second.size();
                Cache.erase(it);
            }
            Cache[k] = CacheEntry{ std::move(tmp), w, h };
            CacheBytes += Cache[k].size();
            if (GVerbose) std::fprintf(stderr, "[tex] loaded: %s (%ux%u)\n", k.c_str(), w, h);
            rgbaOut = Cache[k].rgba; outW = w; outH = h;
        }
        return true;
    };

    if (png && tryFile(p1)) return true;
    if (tryFile(p2)) return true;
    if (!png) {
        // If PNG wasn’t our default, still try it
        if (tryFile(p1)) return true;
    }
    return false;
}

std::string ExtractNdsGameCodeFromRom(const fs::path& romPath) {
    std::ifstream f(romPath, std::ios::binary);
    if (!f) return {};
    f.seekg(0x0C, std::ios::beg);
    char code[4];
    f.read(code, 4);
    if (!f) return {};
    std::string s(code, code+4);
    // sanitize
    for (char& c : s) if (c < 32 || c > 126) c = '_';
    return s;
}

bool DumpEnabled() { return G.enableDump; }
bool ReplaceEnabled() { return G.enableReplace; }

} // namespace melonDS::hires
