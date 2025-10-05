// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpriteDump.h"
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <fstream>

#if SPRITEDUMP_WITH_STB
  #define STB_IMAGE_WRITE_IMPLEMENTATION
  #include "thirdparty/stb/stb_image_write.h"
#endif

namespace fs = std::filesystem;
namespace melonDS::sprites {

static SpriteDumpConfig G;
static std::string GGameId;
static std::unordered_set<std::string> Seen;
struct CacheEntry { std::vector<uint8_t> rgba; uint32_t w=0, h=0; size_t size() const { return rgba.size(); } };
static std::unordered_map<std::string, CacheEntry> Cache; // cache by absolute filename

static inline uint64_t fnv1a64(const void* data, size_t len, uint64_t seed = 1469598103934665603ull) {
    const uint8_t* p = (const uint8_t*)data; uint64_t h=seed; for (size_t i=0;i<len;i++){ h ^= p[i]; h *= 1099511628211ull; } return h;
}
static inline std::string to_hex(uint64_t x){ static const char* d="0123456789abcdef"; std::string s(16,'0'); for(int i=15;i>=0;--i){ s[i]=d[x&0xF]; x>>=4;} return s; }

void Init(const SpriteDumpConfig& cfg, const std::string& gameId){ G=cfg; GGameId=gameId; Seen.clear(); Cache.clear(); }
void Shutdown(){ Seen.clear(); Cache.clear(); }

SpriteKey MakeKey(const uint8_t* rgba, uint32_t w, uint32_t h, ObjFmt fmt){ uint64_t h1=fnv1a64(rgba, size_t(w)*h*4); return SpriteKey{h1,w,h,fmt}; }

static fs::path GameDumpDir(){ return G.dumpDir / (GGameId.empty()? fs::path("Unknown"): fs::path(GGameId)); }
static fs::path GameLoadDir(){ return G.loadDir / (GGameId.empty()? fs::path("Unknown"): fs::path(GGameId)); }

static const char* fmt_name(ObjFmt f){ switch(f){ case ObjFmt::Pal16:return "pal16"; case ObjFmt::Pal256:return "pal256"; case ObjFmt::Bitmap:return "bitmap"; default:return "unk"; } }

std::string KeyToFilename(const SpriteKey& key, bool pngExt){ std::string n="obj1_"+std::to_string(key.width)+"x"+std::to_string(key.height)+"_"+to_hex(key.hash64)+"_"+fmt_name(key.fmt); n += pngExt? ".png":".tga"; return n; }

static bool write_tga(const fs::path& p, const uint8_t* rgba, uint32_t w, uint32_t h){
    std::vector<uint8_t> buf; buf.reserve(18 + size_t(w)*h*4);
    uint8_t hdr[18]{}; hdr[2]=2; hdr[12]=uint8_t(w&0xFF); hdr[13]=uint8_t((w>>8)&0xFF); hdr[14]=uint8_t(h&0xFF); hdr[15]=uint8_t((h>>8)&0xFF); hdr[16]=32; hdr[17]=8|0x20; buf.insert(buf.end(), hdr, hdr+18);
    size_t N = size_t(w)*h; buf.resize(18+N*4); uint8_t* out = buf.data()+18; for(size_t i=0;i<N;i++){ out[i*4+0]=rgba[i*4+2]; out[i*4+1]=rgba[i*4+1]; out[i*4+2]=rgba[i*4+0]; out[i*4+3]=rgba[i*4+3]; }
    std::error_code ec; fs::create_directories(p.parent_path(), ec); std::ofstream f(p, std::ios::binary); if(!f) return false; f.write((const char*)buf.data(), buf.size()); return (bool)f;
}

void DumpIfEnabled(const SpriteKey& key, const uint8_t* rgba, uint32_t w, uint32_t h){
    if (!G.enableDump) return;
    bool png = G.writePNG;
    fs::path dst = GameDumpDir() / KeyToFilename(key, png);
    std::string s = dst.string(); if (Seen.count(s)) return; std::error_code ec; if (fs::exists(dst, ec)) { Seen.insert(s); return; }
#if SPRITEDUMP_WITH_STB
    if (png){ fs::create_directories(dst.parent_path(), ec); stbi_write_png(dst.string().c_str(), w, h, 4, rgba, int(w*4)); Seen.insert(s); return; }
#endif
    if (write_tga(dst, rgba, w, h)) Seen.insert(s);
}

// Minimal TGA reader (BGRA24/32) reused from TexDump.cpp, adapted to local namespace
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
    // RLE
    uint32_t x=0,y=0;
    while (y < h && f) {
        uint8_t packet = uint8_t(f.get());
        if (!f) return false;
        uint8_t count = (packet & 0x7F) + 1;
        if (packet & 0x80) {
            uint8_t b=0,g=0,r=0,a=255;
            b = uint8_t(f.get()); g = uint8_t(f.get()); r = uint8_t(f.get()); if (bpp==32) a = uint8_t(f.get()); if (!f) return false;
            for (uint8_t i=0;i<count;i++) { put_pixel(x,y,b,g,r,a); if (++x>=w){ x=0; if(++y>=h) break; } }
        } else {
            for (uint8_t i=0;i<count;i++) { uint8_t b=0,g=0,r=0,a=255; b=uint8_t(f.get()); g=uint8_t(f.get()); r=uint8_t(f.get()); if (bpp==32) a=uint8_t(f.get()); if (!f) return false; put_pixel(x,y,b,g,r,a); if(++x>=w){ x=0; if(++y>=h) break; } }
        }
    }
    return true;
}

bool TryLoadReplacement(const SpriteKey& key, std::vector<uint8_t>& rgbaOut, uint32_t& outW, uint32_t& outH)
{
    if (!G.enableReplace) return false;
    const bool png = G.writePNG;
    fs::path base = GameLoadDir();
    fs::path p1 = base / KeyToFilename(key, true);
    fs::path p2 = base / KeyToFilename(key, false);

    auto tryFile = [&](const fs::path& p)->bool {
        const std::string k = p.string();
        auto it = Cache.find(k);
        if (it != Cache.end()) { rgbaOut = it->second.rgba; outW = it->second.w; outH = it->second.h; return true; }
        std::error_code ec; if (!fs::exists(p, ec)) return false;
        uint32_t w=0,h=0; std::vector<uint8_t> tmp;
#if SPRITEDUMP_WITH_STB
        if (p.extension() == ".png") {
            int W,H,Comp;
            unsigned char* data = stbi_load(p.string().c_str(), &W, &H, &Comp, 4);
            if (!data) return false;
            tmp.assign(data, data + size_t(W)*H*4); stbi_image_free(data);
            w = uint32_t(W); h = uint32_t(H);
        } else
#endif
        {
            if (!read_tga(p, tmp, w, h)) return false;
        }
        Cache[k] = CacheEntry{ std::move(tmp), w, h };
        rgbaOut = Cache[k].rgba; outW = w; outH = h; return true;
    };

    if (png && tryFile(p1)) return true;
    if (tryFile(p2)) return true;
    if (!png) { if (tryFile(p1)) return true; }
    return false;
}

bool DumpEnabled(){ return G.enableDump; }
bool ReplaceEnabled(){ return G.enableReplace; }
bool SwapRBEnabled(){ return G.swapRB; }

} // namespace
