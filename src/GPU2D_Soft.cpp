/*
    Copyright 2016-2025 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU2D_Soft.h"
#include "GPU.h"
#include "GPU3D.h"
#include "video/hirez/SpriteDump.h"

#include <vector>
#include <array>
#include <algorithm>

namespace melonDS
{
namespace GPU2D
{

static inline void StoreRGBA(std::vector<uint8_t>& buf, u32 width, u32 x, u32 y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x >= width) return;
    size_t idx = (size_t(y) * width + x) * 4;
    if (idx + 3 >= buf.size()) return;
    buf[idx + 0] = r;
    buf[idx + 1] = g;
    buf[idx + 2] = b;
    buf[idx + 3] = a;
}

static inline void Color555ToRGBA(u16 color, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a)
{
    r = uint8_t(((color & 0x001F) * 255 + 15) / 31);
    g = uint8_t((((color & 0x03E0) >> 5) * 255 + 15) / 31);
    b = uint8_t((((color & 0x7C00) >> 10) * 255 + 15) / 31);
    a = (color & 0x8000) ? 255 : 0;
}

static inline u16 RGBA8To5551(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a < 32) return 0; // treat as fully transparent
    u16 out = 0x8000;
    out |= u16((int(r) * 31 + 127) / 255);
    out |= u16((int(g) * 31 + 127) / 255) << 5;
    out |= u16((int(b) * 31 + 127) / 255) << 10;
    return out;
}

bool SoftRenderer::DecodeSpriteForDump(Unit& unit, u16 attr0, u16 attr1, u16 attr2,
                                       u32 width, u32 height, std::vector<uint8_t>& rgbaOut,
                                       melonDS::sprites::ObjFmt& fmtOut)
{
    if (width == 0 || height == 0)
    {
        rgbaOut.clear();
        return false;
    }

    const bool rotscale = (attr0 & 0x0100) != 0;
    if (rotscale)
    {
        rgbaOut.clear();
        return false;
    }

    if (((attr0 >> 10) & 0x3) == 3)
        fmtOut = melonDS::sprites::ObjFmt::Bitmap;
    else if (attr0 & 0x2000)
        fmtOut = melonDS::sprites::ObjFmt::Pal256;
    else
        fmtOut = melonDS::sprites::ObjFmt::Pal16;

    rgbaOut.assign(size_t(width) * height * 4, 0);

    u8* objvram;
    u32 objvrammask;
    unit.GetOBJVRAM(objvram, objvrammask);

    const u32 dispCnt = unit.DispCnt;
    const bool useExtPal = (dispCnt & 0x80000000) != 0;
    u16* basePal = (u16*)&GPU.Palette[unit.Num ? 0x600 : 0x200];
    u16* extPal = useExtPal ? unit.GetOBJExtPal() : nullptr;

    const bool xflip = (attr1 & 0x1000) != 0;
    const bool yflip = (attr1 & 0x2000) != 0;

    const u32 tilenum = attr2 & 0x03FF;

    const auto writePixel = [&](u32 destX, u32 destY, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        if (destX >= width || destY >= height) return;
        size_t idx = (size_t(destY) * width + destX) * 4;
        rgbaOut[idx + 0] = r;
        rgbaOut[idx + 1] = g;
        rgbaOut[idx + 2] = b;
        rgbaOut[idx + 3] = a;
    };

    switch (fmtOut)
    {
    case melonDS::sprites::ObjFmt::Bitmap:
    {
        for (u32 destY = 0; destY < height; ++destY)
        {
            u32 srcY = yflip ? (height - 1 - destY) : destY;

            u32 pixelsaddr = tilenum;
            if (dispCnt & 0x40)
            {
                if (dispCnt & 0x20)
                    continue;
                pixelsaddr <<= (7 + ((dispCnt >> 22) & 0x1));
                pixelsaddr += (srcY * width * 2);
            }
            else
            {
                if (dispCnt & 0x20)
                {
                    pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                    pixelsaddr += (srcY * 256 * 2);
                }
                else
                {
                    pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                    pixelsaddr += (srcY * 128 * 2);
                }
            }

            s32 addr = pixelsaddr;
            if (xflip)
                addr += ((width - 1) << 1);

            for (u32 destX = 0; destX < width; ++destX)
            {
                u16 color = *(u16*)&objvram[addr & objvrammask];
                addr += xflip ? -2 : 2;
                uint8_t r = 0, g = 0, b = 0, a = 0;
                if (color & 0x8000)
                    Color555ToRGBA(color, r, g, b, a);
                writePixel(destX, destY, r, g, b, a);
            }
        }
        break;
    }
    case melonDS::sprites::ObjFmt::Pal256:
    {
        const u32 wmask = width - 8;
        const bool oneDim = (dispCnt & 0x10) != 0;
        const u32 oneDimShift = (dispCnt >> 20) & 0x3;
        const bool doubleSize = (attr0 & 0x2000) != 0;
        const u32 palBank256 = (attr2 & 0xF000) >> 4;

        for (u32 destY = 0; destY < height; ++destY)
        {
            u32 srcY = yflip ? (height - 1 - destY) : destY;

            u32 base = tilenum;
            if (oneDim)
            {
                base <<= oneDimShift;
                base += ((srcY >> 3) * (width >> 3)) << (doubleSize ? 1 : 0);
            }
            else
            {
                base += ((srcY >> 3) * 0x20);
            }

            s32 addr = (base << 5) + ((srcY & 0x7) << 3);
            s32 pixelstride;
            if (xflip)
            {
                addr += (((width - 1) & wmask) << 3);
                addr += ((width - 1) & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelstride = 1;
            }

            for (u32 destX = 0; destX < width; ++destX)
            {
                u8 colorIdx = objvram[addr & objvrammask];
                addr += pixelstride;
                if (((destX + 1) & 0x7) == 0)
                    addr += (56 * pixelstride);

                uint8_t r = 0, g = 0, b = 0, a = 0;
                if (colorIdx != 0)
                {
                    u16 palColor = 0;
                    if (useExtPal && extPal)
                        palColor = extPal[palBank256 + colorIdx];
                    else
                        palColor = basePal[colorIdx];
                    Color555ToRGBA(palColor | 0x8000, r, g, b, a);
                }

                writePixel(destX, destY, r, g, b, a);
            }
        }
        break;
    }
    case melonDS::sprites::ObjFmt::Pal16:
    {
        const u32 wmask = width - 8;
        const bool oneDim = (dispCnt & 0x10) != 0;
        const u32 oneDimShift = (dispCnt >> 20) & 0x3;
        const bool doubleSize = (attr0 & 0x2000) != 0;
        const u32 palBank16 = (attr2 >> 12) & 0xF;
        const u32 palBank16Ext = (attr2 & 0xF000) >> 8;

        for (u32 destY = 0; destY < height; ++destY)
        {
            u32 srcY = yflip ? (height - 1 - destY) : destY;

            u32 base = tilenum;
            if (oneDim)
            {
                base <<= oneDimShift;
                base += ((srcY >> 3) * (width >> 3)) << (doubleSize ? 1 : 0);
            }
            else
            {
                base += ((srcY >> 3) * 0x20);
            }

            s32 addr = (base << 5) + ((srcY & 0x7) << 2);
            if (xflip)
            {
                addr += (((width - 1) & wmask) << 2);
                addr += (((width - 1) & 0x7) >> 1);
            }

            for (u32 destX = 0; destX < width; ++destX)
            {
                u8 color;
                if (xflip)
                {
                    if (destX & 0x1)
                    {
                        color = objvram[addr & objvrammask] & 0x0F;
                        addr--;
                    }
                    else
                    {
                        color = objvram[addr & objvrammask] >> 4;
                    }
                }
                else
                {
                    if (destX & 0x1)
                    {
                        color = objvram[addr & objvrammask] >> 4;
                        addr++;
                    }
                    else
                    {
                        color = objvram[addr & objvrammask] & 0x0F;
                    }
                }

                if (((destX + 1) & 0x7) == 0)
                    addr += xflip ? -28 : 28;

                uint8_t r = 0, g = 0, b = 0, a = 0;
                if (color != 0)
                {
                    u16 palColor = 0;
                    if (useExtPal && extPal)
                        palColor = extPal[palBank16Ext + color];
                    else
                        palColor = basePal[(palBank16 << 4) | color];
                    Color555ToRGBA(palColor | 0x8000, r, g, b, a);
                }

                writePixel(destX, destY, r, g, b, a);
            }
        }
        break;
    }
    default:
        rgbaOut.clear();
        return false;
    }

    return true;
}

SoftRenderer::SoftRenderer(melonDS::GPU& gpu)
    : Renderer2D(), GPU(gpu)
{
    // mosaic table is initialized at compile-time
}

u32 SoftRenderer::ColorComposite(int i, u32 val1, u32 val2) const
{
    u32 coloreffect = 0;
    u32 eva, evb;

    u32 flag1 = val1 >> 24;
    u32 flag2 = val2 >> 24;

    u32 blendCnt = CurUnit->BlendCnt;

    u32 target2;
    if      (flag2 & 0x80) target2 = 0x1000;
    else if (flag2 & 0x40) target2 = 0x0100;
    else                   target2 = flag2 << 8;

    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        // sprite blending

        coloreffect = 1;

        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        else
        {
            eva = CurUnit->EVA;
            evb = CurUnit->EVB;
        }
    }
    else if ((flag1 & 0x40) && (blendCnt & target2))
    {
        // 3D layer blending

        coloreffect = 4;
    }
    else
    {
        if      (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;

        if ((blendCnt & flag1) && (WindowMask[i] & 0x20))
        {
            coloreffect = (blendCnt >> 6) & 0x3;

            if (coloreffect == 1)
            {
                if (blendCnt & target2)
                {
                    eva = CurUnit->EVA;
                    evb = CurUnit->EVB;
                }
                else
                    coloreffect = 0;
            }
        }
    }

    switch (coloreffect)
    {
    case 0: return val1;
    case 1: return ColorBlend4(val1, val2, eva, evb);
    case 2: return ColorBrightnessUp(val1, CurUnit->EVY, 0x8);
    case 3: return ColorBrightnessDown(val1, CurUnit->EVY, 0x7);
    case 4: return ColorBlend5(val1, val2);
    }

    return val1;
}

void SoftRenderer::DrawScanline(u32 line, Unit* unit)
{
    CurUnit = unit;

    int stride = GPU.GPU3D.IsRendererAccelerated() ? (256*3 + 1) : 256;
    u32* dst = &Framebuffer[CurUnit->Num][stride * line];

    int n3dline = line;
    line = GPU.VCount;

    if (CurUnit->Num == 0)
    {
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
    }
    else
    {
        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
    }

    bool forceblank = false;

    // scanlines that end up outside of the GPU drawing range
    // (as a result of writing to VCount) are filled white
    if (line > 192) forceblank = true;

    // GPU B can be completely disabled by POWCNT1
    // oddly that's not the case for GPU A
    if (CurUnit->Num && !CurUnit->Enabled) forceblank = true;

    if (line == 0 && CurUnit->CaptureCnt & (1 << 31) && !forceblank)
        CurUnit->CaptureLatch = true;

    if (CurUnit->Num == 0)
    {
        if (!GPU.GPU3D.IsRendererAccelerated())
            _3DLine = GPU.GPU3D.GetLine(n3dline);
        else if (CurUnit->CaptureLatch && (((CurUnit->CaptureCnt >> 29) & 0x3) != 1))
        {
            _3DLine = GPU.GPU3D.GetLine(n3dline);
            //GPU3D::GLRenderer::PrepareCaptureFrame();
        }
    }

    if (forceblank)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFFFFFFFF;

        if (GPU.GPU3D.IsRendererAccelerated())
        {
            dst[256*3] = 0;
        }
        return;
    }

    u32 dispmode = CurUnit->DispCnt >> 16;
    dispmode &= (CurUnit->Num ? 0x1 : 0x3);

    // always render regular graphics
    DrawScanline_BGOBJ(line);
    CurUnit->UpdateMosaicCounters(line);

    switch (dispmode)
    {
    case 0: // screen off
        {
            for (int i = 0; i < 256; i++)
                dst[i] = 0x003F3F3F;
        }
        break;

    case 1: // regular display
        {
            int i = 0;
            for (; i < (stride & ~1); i+=2)
                *(u64*)&dst[i] = *(u64*)&BGOBJLine[i];
        }
        break;

    case 2: // VRAM display
        {
            u32 vrambank = (CurUnit->DispCnt >> 18) & 0x3;
            if (GPU.VRAMMap_LCDC & (1<<vrambank))
            {
                u16* vram = (u16*)GPU.VRAM[vrambank];
                vram = &vram[line * 256];

                for (int i = 0; i < 256; i++)
                {
                    u16 color = vram[i];
                    u8 r = (color & 0x001F) << 1;
                    u8 g = (color & 0x03E0) >> 4;
                    u8 b = (color & 0x7C00) >> 9;

                    dst[i] = r | (g << 8) | (b << 16);
                }
            }
            else
            {
                for (int i = 0; i < 256; i++)
                {
                    dst[i] = 0;
                }
            }
        }
        break;

    case 3: // FIFO display
        {
            for (int i = 0; i < 256; i++)
            {
                u16 color = CurUnit->DispFIFOBuffer[i];
                u8 r = (color & 0x001F) << 1;
                u8 g = (color & 0x03E0) >> 4;
                u8 b = (color & 0x7C00) >> 9;

                dst[i] = r | (g << 8) | (b << 16);
            }
        }
        break;
    }

    // capture
    if ((CurUnit->Num == 0) && CurUnit->CaptureLatch)
    {
        u32 capwidth, capheight;
        switch ((CurUnit->CaptureCnt >> 20) & 0x3)
        {
        case 0: capwidth = 128; capheight = 128; break;
        case 1: capwidth = 256; capheight = 64;  break;
        case 2: capwidth = 256; capheight = 128; break;
        case 3: capwidth = 256; capheight = 192; break;
        }

        if (line < capheight)
            DoCapture(line, capwidth);
    }

    u32 masterBrightness = CurUnit->MasterBrightness;

    if (GPU.GPU3D.IsRendererAccelerated())
    {
        u32 xpos = GPU.GPU3D.GetRenderXPos();

        dst[256*3] = masterBrightness |
                     (CurUnit->DispCnt & 0x30000) |
                     (xpos << 24) | ((xpos & 0x100) << 15);
        return;
    }

    // master brightness
    if (dispmode != 0)
    {
        if ((masterBrightness >> 14) == 1)
        {
            // up
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessUp(dst[i], factor, 0x0);
            }
        }
        else if ((masterBrightness >> 14) == 2)
        {
            // down
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessDown(dst[i], factor, 0xF);
            }
        }
    }

    // convert to 32-bit BGRA
    // note: 32-bit RGBA would be more straightforward, but
    // BGRA seems to be more compatible (Direct2D soft, cairo...)
    for (int i = 0; i < 256; i+=2)
    {
        u64 c = *(u64*)&dst[i];

        u64 r = (c << 18) & 0xFC000000FC0000;
        u64 g = (c << 2) & 0xFC000000FC00;
        u64 b = (c >> 14) & 0xFC000000FC;
        c = r | g | b;

        *(u64*)&dst[i] = c | ((c & 0x00C0C0C000C0C0C0) >> 6) | 0xFF000000FF000000;
    }
}

void SoftRenderer::VBlankEnd(Unit* unitA, Unit* unitB)
{
    auto processUnit = [&](Unit* unit, int idx)
    {
        if (!unit) return;
        const bool doDump = melonDS::sprites::DumpEnabled();
        const bool doReplace = melonDS::sprites::ReplaceEnabled();
        if (!doDump && !doReplace)
            return;

        static const u32 spritewidth[16] =
        {
            8, 16, 8, 8,
            16, 32, 8, 8,
            32, 32, 16, 8,
            64, 64, 32, 8
        };
        static const u32 spriteheight[16] =
        {
            8, 8, 16, 8,
            16, 8, 32, 8,
            32, 16, 32, 8,
            64, 32, 64, 8
        };

        u16* oam = (u16*)&GPU.OAM[idx ? 0x400 : 0];
        auto& replArray = SpriteReplacement[idx];

        for (int i = 0; i < 128; ++i)
        {
            u16 attr0 = oam[i*4 + 0];
            u16 attr1 = oam[i*4 + 1];
            u16 attr2 = oam[i*4 + 2];

            auto& replState = replArray[i];
            replState.hasReplacement = false;

            if ((attr0 & 0x0300) == 0x0200)
                continue;

            bool rotscale = (attr0 & 0x0100) != 0;
            u32 sizeparam = (attr0 >> 14) | ((attr1 & 0xC000) >> 12);
            if (sizeparam >= 16)
                continue;

            u32 width = spritewidth[sizeparam];
            u32 height = spriteheight[sizeparam];

            if (rotscale && (attr0 & 0x0200))
            {
                width <<= 1;
                height <<= 1;
            }

            std::vector<uint8_t> rgba;
            melonDS::sprites::ObjFmt fmt;
            if (!DecodeSpriteForDump(*unit, attr0, attr1, attr2, width, height, rgba, fmt))
                continue;

            if (fmt == melonDS::sprites::ObjFmt::Bitmap)
            {
                // Skip direct-color sprites (typically 3D capture surfaces)
                replState.hasReplacement = false;
                continue;
            }

            if (doDump && !rgba.empty())
            {
                auto key = melonDS::sprites::MakeKey(rgba.data(), width, height, fmt);
                melonDS::sprites::DumpIfEnabled(key, rgba.data(), width, height);
            }

            if (doReplace && !rotscale)
            {
                auto loadIntoState = [&](const std::vector<uint8_t>& keyRgba, bool adjustForFlip) -> bool
                {
                    std::vector<uint8_t> replData;
                    u32 rw = width;
                    u32 rh = height;
                    auto key = melonDS::sprites::MakeKey(keyRgba.data(), width, height, fmt);
                    if (!melonDS::sprites::TryLoadReplacement(key, replData, rw, rh))
                        return false;
                    if (rw % width || rh % height)
                        return false;

                    u32 scaleX = rw / width;
                    u32 scaleY = rh / height;
                    bool swapRB = melonDS::sprites::SwapRBEnabled();

                    replState.colors.resize(size_t(width) * height);
                    for (u32 y = 0; y < height; ++y)
                    {
                        for (u32 x = 0; x < width; ++x)
                        {
                            u32 sampleX = x * scaleX;
                            u32 sampleY = y * scaleY;
                            if (adjustForFlip)
                            {
                                if (attr1 & 0x1000)
                                    sampleX = rw - scaleX * (x + 1);
                                if (attr1 & 0x2000)
                                    sampleY = rh - scaleY * (y + 1);
                            }
                            if (sampleX >= rw) sampleX = rw - 1;
                            if (sampleY >= rh) sampleY = rh - 1;
                            size_t srcIndex = (size_t(sampleY) * rw + sampleX) * 4;
                            if (srcIndex + 3 >= replData.size())
                            {
                                replState.colors[y*width + x] = 0;
                                continue;
                            }
                            uint8_t r = replData[srcIndex + (swapRB ? 2 : 0)];
                            uint8_t g = replData[srcIndex + 1];
                            uint8_t b = replData[srcIndex + (swapRB ? 0 : 2)];
                            uint8_t a = replData[srcIndex + 3];
                            replState.colors[y*width + x] = RGBA8To5551(r, g, b, a);
                        }
                    }
                    replState.width = width;
                    replState.height = height;
                    replState.hasReplacement = true;
                    return true;
                };

                bool loaded = loadIntoState(rgba, false);
                if (!loaded && ((attr1 & 0x3000) != 0))
                {
                    std::vector<uint8_t> alt = rgba;
                    if (attr1 & 0x1000)
                    {
                        for (u32 y = 0; y < height; ++y)
                        {
                            uint8_t* row = &alt[(size_t(y) * width) * 4];
                            for (u32 x = 0; x < width / 2; ++x)
                            {
                                for (int c = 0; c < 4; ++c)
                                    std::swap(row[x*4 + c], row[(width-1-x)*4 + c]);
                            }
                        }
                    }
                    if (attr1 & 0x2000)
                    {
                        for (u32 y = 0; y < height / 2; ++y)
                        {
                            uint8_t* rowTop = &alt[(size_t(y) * width) * 4];
                            uint8_t* rowBottom = &alt[(size_t(height-1-y) * width) * 4];
                            for (u32 x = 0; x < width * 4; ++x)
                                std::swap(rowTop[x], rowBottom[x]);
                        }
                    }
                    loaded = loadIntoState(alt, true);
                }
            }
        }
    };

    processUnit(unitA, 0);
    processUnit(unitB, 1);

#ifdef OGLRENDERER_ENABLED
    if (Renderer3D& renderer3d = GPU.GPU3D.GetCurrentRenderer(); renderer3d.Accelerated)
    {
        if ((unitA->CaptureCnt & (1<<31)) && (((unitA->CaptureCnt >> 29) & 0x3) != 1))
        {
            renderer3d.PrepareCaptureFrame();
        }
    }
#endif
}

void SoftRenderer::DoCapture(u32 line, u32 width)
{
    u32 captureCnt = CurUnit->CaptureCnt;
    u32 dstvram = (captureCnt >> 16) & 0x3;

    // TODO: confirm this
    // it should work like VRAM display mode, which requires VRAM to be mapped to LCDC
    if (!(GPU.VRAMMap_LCDC & (1<<dstvram)))
        return;

    u16* dst = (u16*)GPU.VRAM[dstvram];
    u32 dstaddr = (((captureCnt >> 18) & 0x3) << 14) + (line * width);

    // TODO: handle 3D in GPU3D::CurrentRenderer->Accelerated mode!!

    u32* srcA;
    if (captureCnt & (1<<24))
    {
        srcA = _3DLine;
    }
    else
    {
        srcA = BGOBJLine;
        if (GPU.GPU3D.IsRendererAccelerated())
        {
            // in GPU3D::CurrentRenderer->Accelerated mode, compositing is normally done on the GPU
            // but when doing display capture, we do need the composited output
            // so we do it here

            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 compmode = (val3 >> 24) & 0xF;

                if (compmode == 4)
                {
                    // 3D on top, blending

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                        val1 = ColorBlend5(_3dval, val1);
                    else
                        val1 = val2;
                }
                else if (compmode == 1)
                {
                    // 3D on bottom, blending

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 eva = (val3 >> 8) & 0x1F;
                        u32 evb = (val3 >> 16) & 0x1F;

                        val1 = ColorBlend4(val1, _3dval, eva, evb);
                    }
                    else
                        val1 = val2;
                }
                else if (compmode <= 3)
                {
                    // 3D on top, normal/fade

                    u32 _3dval = _3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 evy = (val3 >> 8) & 0x1F;

                        val1 = _3dval;
                        if      (compmode == 2) val1 = ColorBrightnessUp(val1, evy, 0x8);
                        else if (compmode == 3) val1 = ColorBrightnessDown(val1, evy, 0x7);
                    }
                    else
                        val1 = val2;
                }

                BGOBJLine[i] = val1;
            }
        }
    }

    u16* srcB = NULL;
    u32 srcBaddr = line * 256;

    if (captureCnt & (1<<25))
    {
        srcB = &CurUnit->DispFIFOBuffer[0];
        srcBaddr = 0;
    }
    else
    {
        u32 srcvram = (CurUnit->DispCnt >> 18) & 0x3;
        if (GPU.VRAMMap_LCDC & (1<<srcvram))
            srcB = (u16*)GPU.VRAM[srcvram];

        if (((CurUnit->DispCnt >> 16) & 0x3) != 2)
            srcBaddr += ((captureCnt >> 26) & 0x3) << 14;
    }

    dstaddr &= 0xFFFF;
    srcBaddr &= 0xFFFF;

    static_assert(VRAMDirtyGranularity == 512);
    GPU.VRAMDirty[dstvram][(dstaddr * 2) / VRAMDirtyGranularity] = true;

    switch ((captureCnt >> 29) & 0x3)
    {
    case 0: // source A
        {
            for (u32 i = 0; i < width; i++)
            {
                u32 val = srcA[i];

                // TODO: check what happens when alpha=0

                u32 r = (val >> 1) & 0x1F;
                u32 g = (val >> 9) & 0x1F;
                u32 b = (val >> 17) & 0x1F;
                u32 a = ((val >> 24) != 0) ? 0x8000 : 0;

                dst[dstaddr] = r | (g << 5) | (b << 10) | a;
                dstaddr = (dstaddr + 1) & 0xFFFF;
            }
        }
        break;

    case 1: // source B
        {
            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = srcB[srcBaddr];
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = 0;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;

    case 2: // sources A+B
    case 3:
        {
            u32 eva = captureCnt & 0x1F;
            u32 evb = (captureCnt >> 8) & 0x1F;

            // checkme
            if (eva > 16) eva = 16;
            if (evb > 16) evb = 16;

            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    val = srcB[srcBaddr];

                    u32 rB = val & 0x1F;
                    u32 gB = (val >> 5) & 0x1F;
                    u32 bB = (val >> 10) & 0x1F;
                    u32 aB = val >> 15;

                    u32 rD = ((rA * aA * eva) + (rB * aB * evb) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + (gB * aB * evb) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + (bB * aB * evb) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0) | (evb>0 ? aB : 0);

                    if (rD > 0x1F) rD = 0x1F;
                    if (gD > 0x1F) gD = 0x1F;
                    if (bD > 0x1F) bD = 0x1F;

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    u32 rD = ((rA * aA * eva) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0);

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;
    }
}

#define DoDrawBG(type, line, num) \
    do \
    { \
        if ((bgCnt[num] & 0x0040) && (CurUnit->BGMosaicSize[0] > 0)) \
        { \
            if (GPU.GPU3D.IsRendererAccelerated()) DrawBG_##type<true, DrawPixel_Accel>(line, num); \
            else DrawBG_##type<true, DrawPixel_Normal>(line, num); \
        } \
        else \
        { \
            if (GPU.GPU3D.IsRendererAccelerated()) DrawBG_##type<false, DrawPixel_Accel>(line, num); \
            else DrawBG_##type<false, DrawPixel_Normal>(line, num); \
        } \
    } while (false)

#define DoDrawBG_Large(line) \
    do \
    { \
        if ((bgCnt[2] & 0x0040) && (CurUnit->BGMosaicSize[0] > 0)) \
        { \
            if (GPU.GPU3D.IsRendererAccelerated()) DrawBG_Large<true, DrawPixel_Accel>(line); \
            else DrawBG_Large<true, DrawPixel_Normal>(line); \
        } \
        else \
        { \
            if (GPU.GPU3D.IsRendererAccelerated()) DrawBG_Large<false, DrawPixel_Accel>(line); \
            else DrawBG_Large<false, DrawPixel_Normal>(line); \
        } \
    } while (false)

#define DoInterleaveSprites(prio) \
    if (GPU.GPU3D.IsRendererAccelerated()) InterleaveSprites<DrawPixel_Accel>(prio); else InterleaveSprites<DrawPixel_Normal>(prio);

template<u32 bgmode>
void SoftRenderer::DrawScanlineBGMode(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u16* bgCnt = CurUnit->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[3] & 0x3) == i)
        {
            if (dispCnt & 0x0800)
            {
                if (bgmode >= 3)
                    DoDrawBG(Extended, line, 3);
                else if (bgmode >= 1)
                    DoDrawBG(Affine, line, 3);
                else
                    DoDrawBG(Text, line, 3);
            }
        }
        if ((bgCnt[2] & 0x3) == i)
        {
            if (dispCnt & 0x0400)
            {
                if (bgmode == 5)
                    DoDrawBG(Extended, line, 2);
                else if (bgmode == 4 || bgmode == 2)
                    DoDrawBG(Affine, line, 2);
                else
                    DoDrawBG(Text, line, 2);
            }
        }
        if ((bgCnt[1] & 0x3) == i)
        {
            if (dispCnt & 0x0200)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if (!CurUnit->Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((dispCnt & 0x1000) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16));
        }

    }
}

void SoftRenderer::DrawScanlineBGMode6(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u16* bgCnt = CurUnit->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[2] & 0x3) == i)
        {
            if (dispCnt & 0x0400)
            {
                DoDrawBG_Large(line);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if ((!CurUnit->Num) && (dispCnt & 0x8))
                    DrawBG_3D();
            }
        }
        if ((dispCnt & 0x1000) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanlineBGMode7(u32 line)
{
    u32 dispCnt = CurUnit->DispCnt;
    u16* bgCnt = CurUnit->BGCnt;
    // mode 7 only has text-mode BG0 and BG1

    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[1] & 0x3) == i)
        {
            if (dispCnt & 0x0200)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if (!CurUnit->Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((dispCnt & 0x1000) && NumSprites[CurUnit->Num])
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanline_BGOBJ(u32 line)
{
    // forced blank disables BG/OBJ compositing
    if (CurUnit->DispCnt & (1<<7))
    {
        for (int i = 0; i < 256; i++)
            BGOBJLine[i] = 0xFF3F3F3F;

        return;
    }

    u64 backdrop;
    if (CurUnit->Num) backdrop = *(u16*)&GPU.Palette[0x400];
    else     backdrop = *(u16*)&GPU.Palette[0];

    {
        u8 r = (backdrop & 0x001F) << 1;
        u8 g = (backdrop & 0x03E0) >> 4;
        u8 b = (backdrop & 0x7C00) >> 9;

        backdrop = r | (g << 8) | (b << 16) | 0x20000000;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i+=2)
            *(u64*)&BGOBJLine[i] = backdrop;
    }

    if (CurUnit->DispCnt & 0xE000)
        CurUnit->CalculateWindowMask(line, WindowMask, OBJWindow[CurUnit->Num]);
    else
        memset(WindowMask, 0xFF, 256);

    ApplySpriteMosaicX();
    CurBGXMosaicTable = MosaicTable[CurUnit->BGMosaicSize[0]].data();

    switch (CurUnit->DispCnt & 0x7)
    {
    case 0: DrawScanlineBGMode<0>(line); break;
    case 1: DrawScanlineBGMode<1>(line); break;
    case 2: DrawScanlineBGMode<2>(line); break;
    case 3: DrawScanlineBGMode<3>(line); break;
    case 4: DrawScanlineBGMode<4>(line); break;
    case 5: DrawScanlineBGMode<5>(line); break;
    case 6: DrawScanlineBGMode6(line); break;
    case 7: DrawScanlineBGMode7(line); break;
    }

    // color special effects
    // can likely be optimized

    if (!GPU.GPU3D.IsRendererAccelerated())
    {
        for (int i = 0; i < 256; i++)
        {
            u32 val1 = BGOBJLine[i];
            u32 val2 = BGOBJLine[256+i];

            BGOBJLine[i] = ColorComposite(i, val1, val2);
        }
    }
    else
    {
        if (CurUnit->Num == 0)
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 flag1 = val1 >> 24;
                u32 flag2 = val2 >> 24;

                u32 bldcnteffect = (CurUnit->BlendCnt >> 6) & 0x3;

                u32 target1;
                if      (flag1 & 0x80) target1 = 0x0010;
                else if (flag1 & 0x40) target1 = 0x0001;
                else                   target1 = flag1;

                u32 target2;
                if      (flag2 & 0x80) target2 = 0x1000;
                else if (flag2 & 0x40) target2 = 0x0100;
                else                   target2 = flag2 << 8;

                if (((flag1 & 0xC0) == 0x40) && (CurUnit->BlendCnt & target2))
                {
                    // 3D on top, blending

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(i, val2, val3);
                    BGOBJLine[512+i] = 0x04000000;
                }
                else if ((flag1 & 0xC0) == 0x40)
                {
                    // 3D on top, normal/fade

                    if (bldcnteffect == 1)             bldcnteffect = 0;
                    if (!(CurUnit->BlendCnt & 0x0001)) bldcnteffect = 0;
                    if (!(WindowMask[i] & 0x20))       bldcnteffect = 0;

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(i, val2, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (CurUnit->EVY << 8);
                }
                else if (((flag2 & 0xC0) == 0x40) && ((CurUnit->BlendCnt & 0x01C0) == 0x0140))
                {
                    // 3D on bottom, blending

                    u32 eva, evb;
                    if ((flag1 & 0xC0) == 0xC0)
                    {
                        eva = flag1 & 0x1F;
                        evb = 16 - eva;
                    }
                    else if (((CurUnit->BlendCnt & target1) && (WindowMask[i] & 0x20)) ||
                            ((flag1 & 0xC0) == 0x80))
                    {
                        eva = CurUnit->EVA;
                        evb = CurUnit->EVB;
                    }
                    else
                        bldcnteffect = 7;

                    BGOBJLine[i]     = val1;
                    BGOBJLine[256+i] = ColorComposite(i, val1, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (CurUnit->EVB << 16) | (CurUnit->EVA << 8);
                }
                else
                {
                    // no potential 3D pixel involved

                    BGOBJLine[i]     = ColorComposite(i, val1, val2);
                    BGOBJLine[256+i] = 0;
                    BGOBJLine[512+i] = 0x07000000;
                }
            }
        }
        else
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];

                BGOBJLine[i]     = ColorComposite(i, val1, val2);
                BGOBJLine[256+i] = 0;
                BGOBJLine[512+i] = 0x07000000;
            }
        }
    }

    if (CurUnit->BGMosaicY >= CurUnit->BGMosaicYMax)
    {
        CurUnit->BGMosaicY = 0;
        CurUnit->BGMosaicYMax = CurUnit->BGMosaicSize[1];
    }
    else
        CurUnit->BGMosaicY++;

    /*if (OBJMosaicY >= OBJMosaicYMax)
    {
        OBJMosaicY = 0;
        OBJMosaicYMax = OBJMosaicSize[1];
    }
    else
        OBJMosaicY++;*/
}


void SoftRenderer::DrawPixel_Normal(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = (color & 0x03E0) >> 4;
    u8 b = (color & 0x7C00) >> 9;
    //g |= ((color & 0x8000) >> 15);

    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawPixel_Accel(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = (color & 0x03E0) >> 4;
    u8 b = (color & 0x7C00) >> 9;

    *(dst+512) = *(dst+256);
    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawBG_3D()
{
    int i = 0;

    if (GPU.GPU3D.IsRendererAccelerated())
    {
        for (i = 0; i < 256; i++)
        {
            if (!(WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+512] = BGOBJLine[i+256];
            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = 0x40000000; // 3D-layer placeholder
        }
    }
    else
    {
        for (i = 0; i < 256; i++)
        {
            u32 c = _3DLine[i];

            if ((c >> 24) == 0) continue;
            if (!(WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = c | 0x40000000;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Text(u32 line, u32 bgnum)
{
    // workaround for backgrounds missing on aarch64 with lto build
    asm volatile ("" : : : "memory");

    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal, extpalslot;

    u16 xoff = CurUnit->BGXPos[bgnum];
    u16 yoff = CurUnit->BGYPos[bgnum] + line;

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        yoff -= CurUnit->BGMosaicY;
    }

    u32 widexmask = (bgcnt & 0x4000) ? 0x100 : 0;

    extpal = (CurUnit->DispCnt & 0x40000000);
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    CurUnit->GetBGVRAM(bgvram, bgvrammask);
    if (CurUnit->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    // adjust Y position in tilemap
    if (bgcnt & 0x8000)
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & 0x4000)
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u16 curtile;
    u16* curpal;
    u32 pixelsaddr;
    u8 color;
    u32 lastxpos;

    if (bgcnt & 0x0080)
    {
        // 256-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask];

            if (extpal) curpal = CurUnit->GetBGExtPal(extpalslot, curtile>>12);
            else        curpal = pal;

            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];

                if (extpal) curpal = CurUnit->GetBGExtPal(extpalslot, curtile>>12);
                else        curpal = pal;

                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
    else
    {
        // 16-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[((tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3))) & bgvrammask];
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                if (tilexoff & 0x1)
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4;
                }
                else
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F;
                }

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Affine(u32 line, u32 bgnum)
{
    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;

    u32 coordmask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: coordmask = 0x07800; yshift = 7; break;
    case 0x4000: coordmask = 0x0F800; yshift = 8; break;
    case 0x8000: coordmask = 0x1F800; yshift = 9; break;
    case 0xC000: coordmask = 0x3F800; yshift = 10; break;
    }

    u32 overflowmask;
    if (bgcnt & 0x2000) overflowmask = 0;
    else                overflowmask = ~(coordmask | 0x7FF);

    s16 rotA = CurUnit->BGRotA[bgnum-2];
    s16 rotB = CurUnit->BGRotB[bgnum-2];
    s16 rotC = CurUnit->BGRotC[bgnum-2];
    s16 rotD = CurUnit->BGRotD[bgnum-2];

    s32 rotX = CurUnit->BGXRefInternal[bgnum-2];
    s32 rotY = CurUnit->BGYRefInternal[bgnum-2];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (CurUnit->BGMosaicY * rotB);
        rotY -= (CurUnit->BGMosaicY * rotD);
    }

    u8* bgvram;
    u32 bgvrammask;
    CurUnit->GetBGVRAM(bgvram, bgvrammask);

    if (CurUnit->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    u16 curtile;
    u8 color;

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<bgnum))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if ((!((finalX|finalY) & overflowmask)))
            {
                curtile = bgvram[(tilemapaddr + ((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11))) & bgvrammask];

                // draw pixel
                u32 tilexoff = (finalX >> 8) & 0x7;
                u32 tileyoff = (finalY >> 8) & 0x7;

                color = bgvram[(tilesetaddr + (curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }

    CurUnit->BGXRefInternal[bgnum-2] += rotB;
    CurUnit->BGYRefInternal[bgnum-2] += rotD;
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Extended(u32 line, u32 bgnum)
{
    u16 bgcnt = CurUnit->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal;

    u8* bgvram;
    u32 bgvrammask;
    CurUnit->GetBGVRAM(bgvram, bgvrammask);

    extpal = (CurUnit->DispCnt & 0x40000000);

    s16 rotA = CurUnit->BGRotA[bgnum-2];
    s16 rotB = CurUnit->BGRotB[bgnum-2];
    s16 rotC = CurUnit->BGRotC[bgnum-2];
    s16 rotD = CurUnit->BGRotD[bgnum-2];

    s32 rotX = CurUnit->BGXRefInternal[bgnum-2];
    s32 rotY = CurUnit->BGYRefInternal[bgnum-2];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (CurUnit->BGMosaicY * rotB);
        rotY -= (CurUnit->BGMosaicY * rotD);
    }

    if (bgcnt & 0x0080)
    {
        // bitmap modes

        u32 xmask, ymask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
        case 0x4000: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
        case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        u32 ofxmask, ofymask;
        if (bgcnt & 0x2000)
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        if (CurUnit->Num) tilemapaddr = ((bgcnt & 0x1F00) << 6);
        else              tilemapaddr = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & 0x0004)
        {
            // direct color bitmap

            u16 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = *(u16*)&bgvram[(tilemapaddr + (((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) << 1)) & bgvrammask];

                        if (color & 0x8000)
                            drawPixel(&BGOBJLine[i], color, 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            // 256-color bitmap

            if (CurUnit->Num) pal = (u16*)&GPU.Palette[0x400];
            else              pal = (u16*)&GPU.Palette[0];

            u8 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = bgvram[(tilemapaddr + (((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                        if (color)
                            drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        // mixed affine/text mode

        u32 coordmask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: coordmask = 0x07800; yshift = 7; break;
        case 0x4000: coordmask = 0x0F800; yshift = 8; break;
        case 0x8000: coordmask = 0x1F800; yshift = 9; break;
        case 0xC000: coordmask = 0x3F800; yshift = 10; break;
        }

        u32 overflowmask;
        if (bgcnt & 0x2000) overflowmask = 0;
        else                overflowmask = ~(coordmask | 0x7FF);

        if (CurUnit->Num)
        {
            tilesetaddr = ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0x400];
        }
        else
        {
            tilesetaddr = ((CurUnit->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((CurUnit->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0];
        }

        u16 curtile;
        u16* curpal;
        u8 color;

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (WindowMask[i] & (1<<bgnum))
            {
                s32 finalX, finalY;
                if (mosaic)
                {
                    int im = CurBGXMosaicTable[i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if ((!((finalX|finalY) & overflowmask)))
                {
                    curtile = *(u16*)&bgvram[(tilemapaddr + (((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11)) << 1)) & bgvrammask];

                    if (extpal) curpal = CurUnit->GetBGExtPal(bgnum, curtile>>12);
                    else        curpal = pal;

                    // draw pixel
                    u32 tilexoff = (finalX >> 8) & 0x7;
                    u32 tileyoff = (finalY >> 8) & 0x7;

                    if (curtile & 0x0400) tilexoff = 7-tilexoff;
                    if (curtile & 0x0800) tileyoff = 7-tileyoff;

                    color = bgvram[(tilesetaddr + ((curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                    if (color)
                        drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
                }
            }

            rotX += rotA;
            rotY += rotC;
        }
    }

    CurUnit->BGXRefInternal[bgnum-2] += rotB;
    CurUnit->BGYRefInternal[bgnum-2] += rotD;
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Large(u32 line) // BG is always BG2
{
    u16 bgcnt = CurUnit->BGCnt[2];

    u16* pal;

    // large BG sizes:
    // 0: 512x1024
    // 1: 1024x512
    // 2: 512x256
    // 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
    case 0x4000: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
    case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
    case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    u32 ofxmask, ofymask;
    if (bgcnt & 0x2000)
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    s16 rotA = CurUnit->BGRotA[0];
    s16 rotB = CurUnit->BGRotB[0];
    s16 rotC = CurUnit->BGRotC[0];
    s16 rotD = CurUnit->BGRotD[0];

    s32 rotX = CurUnit->BGXRefInternal[0];
    s32 rotY = CurUnit->BGYRefInternal[0];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (CurUnit->BGMosaicY * rotB);
        rotY -= (CurUnit->BGMosaicY * rotD);
    }

    u8* bgvram;
    u32 bgvrammask;
    CurUnit->GetBGVRAM(bgvram, bgvrammask);

    // 256-color bitmap

    if (CurUnit->Num) pal = (u16*)&GPU.Palette[0x400];
    else     pal = (u16*)&GPU.Palette[0];

    u8 color;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<2))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!(finalX & ofxmask) && !(finalY & ofymask))
            {
                color = bgvram[((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<2);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }

    CurUnit->BGXRefInternal[0] += rotB;
    CurUnit->BGYRefInternal[0] += rotD;
}

// OBJ line buffer:
// * bit0-15: color (bit15=1: direct color, bit15=0: palette index, bit12=0 to indicate extpal)
// * bit16-17: BG-relative priority
// * bit18: non-transparent sprite pixel exists here
// * bit19: X mosaic should be applied here
// * bit24-31: compositor flags

void SoftRenderer::ApplySpriteMosaicX()
{
    // apply X mosaic if needed
    // X mosaic for sprites is applied after all sprites are rendered

    if (CurUnit->OBJMosaicSize[0] == 0) return;

    u32* objLine = OBJLine[CurUnit->Num];

    u8* curOBJXMosaicTable = MosaicTable[CurUnit->OBJMosaicSize[0]].data();

    u32 lastcolor = objLine[0];

    for (u32 i = 1; i < 256; i++)
    {
        u32 currentcolor = objLine[i];

        if (!(lastcolor & currentcolor & 0x100000) || curOBJXMosaicTable[i] == 0)
            lastcolor = currentcolor;
        else
            objLine[i] = lastcolor;
    }
}

template <SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::InterleaveSprites(u32 prio)
{
    u32* objLine = OBJLine[CurUnit->Num];
    u16* pal = (u16*)&GPU.Palette[CurUnit->Num ? 0x600 : 0x200];

    if (CurUnit->DispCnt & 0x80000000)
    {
        u16* extpal = CurUnit->GetOBJExtPal();

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else if (pixel & 0x1000)
                color = pal[pixel & 0xFF];
            else
                color = extpal[pixel & 0xFFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
    else
    {
        // optimized no-extpal version

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else
                color = pal[pixel & 0xFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
}

#define DoDrawSprite(type, ...) \
    if (iswin) \
    { \
        DrawSprite_##type<true>(__VA_ARGS__); \
    } \
    else \
    { \
        DrawSprite_##type<false>(__VA_ARGS__); \
    }

void SoftRenderer::DrawSprites(u32 line, Unit* unit)
{
    CurUnit = unit;

    if (line == 0)
    {
        // reset those counters here
        // TODO: find out when those are supposed to be reset
        // it would make sense to reset them at the end of VBlank
        // however, sprites are rendered one scanline in advance
        // so they need to be reset a bit earlier

        CurUnit->OBJMosaicY = 0;
        CurUnit->OBJMosaicYCount = 0;

    }

    if (CurUnit->Num == 0)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
    }
    else
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
    }

    NumSprites[CurUnit->Num] = 0;
    memset(OBJLine[CurUnit->Num], 0, 256*4);
    memset(OBJWindow[CurUnit->Num], 0, 256);
    if (!(CurUnit->DispCnt & 0x1000)) return;

    u16* oam = (u16*)&GPU.OAM[CurUnit->Num ? 0x400 : 0];

    const s32 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    const s32 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    for (int bgnum = 0x0C00; bgnum >= 0x0000; bgnum -= 0x0400)
    {
        for (int sprnum = 127; sprnum >= 0; sprnum--)
        {
            u16* attrib = &oam[sprnum*4];

            if ((attrib[2] & 0x0C00) != bgnum)
                continue;

            bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

            u32 sprline;
            if ((attrib[0] & 0x1000) && !iswin)
            {
                // apply Y mosaic
                sprline = CurUnit->OBJMosaicY;
            }
            else
                sprline = line;

            if (attrib[0] & 0x0100)
            {
                u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
                s32 width = spritewidth[sizeparam];
                s32 height = spriteheight[sizeparam];
                s32 boundwidth = width;
                s32 boundheight = height;

                if (attrib[0] & 0x0200)
                {
                    boundwidth <<= 1;
                    boundheight <<= 1;
                }

                u32 ypos = attrib[0] & 0xFF;
                if (((line - ypos) & 0xFF) >= (u32)boundheight)
                    continue;
                ypos = (sprline - ypos) & 0xFF;

                s32 xpos = (s32)(attrib[1] << 23) >> 23;
                if (xpos <= -boundwidth)
                    continue;

                u32 rotparamgroup = (attrib[1] >> 9) & 0x1F;

                DoDrawSprite(Rotscale, sprnum, boundwidth, boundheight, width, height, xpos, ypos);

                NumSprites[CurUnit->Num]++;
            }
            else
            {
                if (attrib[0] & 0x0200)
                    continue;

                u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
                s32 width = spritewidth[sizeparam];
                s32 height = spriteheight[sizeparam];

                u32 ypos = attrib[0] & 0xFF;
                if (((line - ypos) & 0xFF) >= (u32)height)
                    continue;
                ypos = (sprline - ypos) & 0xFF;

                s32 xpos = (s32)(attrib[1] << 23) >> 23;
                if (xpos <= -width)
                    continue;

                DoDrawSprite(Normal, sprnum, width, height, xpos, ypos);

                NumSprites[CurUnit->Num]++;
            }
        }
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[CurUnit->Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    CurUnit->GetOBJVRAM(objvram, objvrammask);

    u32* objLine = OBJLine[CurUnit->Num];
    u8* objWindow = OBJWindow[CurUnit->Num];

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((xoff-centerX) * rotA) + ((ypos-centerY) * rotB) + (width << 7);
    s32 rotY = ((xoff-centerX) * rotC) + ((ypos-centerY) * rotD) + (height << 7);

    width <<= 8;
    height <<= 8;

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (CurUnit->DispCnt & 0x40)
        {
            if (CurUnit->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((CurUnit->DispCnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (CurUnit->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < boundwidth;)
        {
            if ((u32)rotX < width && (u32)rotY < height)
            {
                color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];

                if (color & 0x8000)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x10)
        {
            pixelsaddr <<= ((CurUnit->DispCnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & 0x2000)
        {
            // 256-color

            if (!window)
            {
                if (!(CurUnit->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];

                    if (color)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = color | pixelattr;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;

                    if (color)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = color | pixelattr;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[CurUnit->Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 wmask = width - 8; // really ((width - 1) & ~0x7)
    const bool xflip = (attrib[1] & 0x1000) != 0;
    const bool yflip = (attrib[1] & 0x2000) != 0;

    const SpriteReplacementState* replacement = nullptr;
    if (melonDS::sprites::ReplaceEnabled())
    {
        const auto& replState = SpriteReplacement[CurUnit->Num][num];
        if (replState.hasReplacement && replState.width == width && replState.height == height)
            replacement = &replState;
    }

    const u32 directMask = pixelattr & 0xFFFF0000;

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u8* objvram;
    u32 objvrammask;
    CurUnit->GetOBJVRAM(objvram, objvrammask);

    u32* objLine = OBJLine[CurUnit->Num];
    u8* objWindow = OBJWindow[CurUnit->Num];

    const bool useExtPal = (CurUnit->DispCnt & 0x80000000);
    u16* basePal = (u16*)&GPU.Palette[CurUnit->Num ? 0x600 : 0x200];
    u16* extPal = useExtPal ? CurUnit->GetOBJExtPal() : nullptr;
    u32 palBank16 = (attrib[2] >> 12) & 0xF;
    u32 palBank16Ext = (attrib[2] & 0xF000) >> 8;
    u32 palBank256 = (attrib[2] & 0xF000) >> 4;

    const auto sampleReplacement = [&](u32 localX, u32 localY) -> u16
    {
        if (!replacement)
            return 0;
        if (localX >= replacement->width || localY >= replacement->height)
            return 0;
        return replacement->colors[localY * replacement->width + localX];
    };

    // yflip
    if (yflip)
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        // bitmap sprite

        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x40)
        {
            if (CurUnit->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((CurUnit->DispCnt >> 22) & 0x1));
                pixelsaddr += (ypos * width * 2);
            }
        }
        else
        {
            if (CurUnit->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (ypos * 128 * 2);
            }
        }

        s32 pixelstride;

        if (attrib[1] & 0x1000) // xflip
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend;)
        {
            u32 localX = xoff;
            u32 localY = ypos;
            color = *(u16*)&objvram[pixelsaddr & objvrammask];

            pixelsaddr += pixelstride;

            if (replacement)
            {
                u16 replColor = sampleReplacement(localX, localY);
                if (replColor)
                    color = replColor;
                else
                    color = 0;
            }

            if (color & 0x8000)
            {
                if (window) objWindow[xpos] = 1;
                else        objLine[xpos] = replacement ? ((color & 0x7FFF) | 0x8000 | directMask)
                                                        : (color | pixelattr);
            }
            else if (!window)
            {
                if (objLine[xpos] == 0)
                    objLine[xpos] = pixelattr & 0x180000;
            }

            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (CurUnit->DispCnt & 0x10)
        {
            pixelsaddr <<= ((CurUnit->DispCnt >> 20) & 0x3);
            pixelsaddr += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            pixelsaddr += ((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        if (attrib[0] & 0x2000)
        {
            // 256-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 3);
            s32 pixelstride;

            if (!window)
            {
                if (!(CurUnit->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 3);
                pixelsaddr += ((width-1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                u32 localX = xoff;
                u32 localY = ypos;
                color = objvram[pixelsaddr & objvrammask];

                pixelsaddr += pixelstride;

                if (replacement)
                {
                    u16 replColor = sampleReplacement(localX, localY);
                    if (replColor)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = (replColor & 0x7FFF) | 0x8000 | directMask;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }

                    xoff++;
                    xpos++;
                    if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
                    continue;
                }

                if (color)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }
        }
        else
        {
            // 16-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 2);
            s32 pixelstride;

            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            // TODO: optimize VRAM access!!
            // TODO: do xflip better? the 'two pixels per byte' thing makes it a bit shitty

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 2);
                pixelsaddr += (((width-1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                u32 localX = xoff;
                u32 localY = ypos;
                if (attrib[1] & 0x1000)
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr--; }
                    else              color = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr++; }
                    else              color = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                if (replacement)
                {
                    u16 replColor = sampleReplacement(localX, localY);
                    if (replColor)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = (replColor & 0x7FFF) | 0x8000 | directMask;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }

                    xoff++;
                    xpos++;
                    if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
                    continue;
                }

                if (color)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }
        }
    }
}

}
}
