// PC pictures for the keyboard / mouse scheme (ported from the Play tab; not in the game), made when the game first
// loads a picture, through one hook on ALTexture::LoadById:
//  - PC button art (play/ui/pcart.js; [game] pc_button_art): every tutorial reminder and [icon:k] picture, check
//    bubble, window A prompt and menu help strip that shows a Wii Remote / Nunchuk button is drawn again with the key
//    bound to that action (vault entities/menus.md §8f lists them). tools/pcart_gen ran the Play tab's own PcArt on
//    the disc pictures and recorded, per picture, the drawing operations that use the game picture (copies,
//    stretches, mirrors, clears: geometry only) and a layer with only the keyglyph.js glyphs (pc_art.inc). Here the
//    operations are replayed on the user's disc picture and the glyph layer is laid over it. A picture whose size
//    differs from the one recorded (another language's pack) keeps the game's art. 44 are ALIGs and get a new
//    texture; the check bubbles 0xA00490 / 93 / 94 / 95 are single-frame ALTX atlases with a centre pivot, so the
//    atlas texture the game built keeps its frame table and gets the new image through SetImage.
//  - The tutorial texts that name a button twice or by a press the PC key does differently (pcart.js TEXT,
//    WinComText rows 2 and 4) are reworded while the tutorial window copies its row (LayoutText, wstr::Copy).
//
// PC PlayGuide pictures (ported from the Play tab's play/ui/pcguide.js; not in the game). The fishing pictures
// 0xA00499-0xA0049C and the tool pictures 0xA00498 / 0xA0049D / 0xA0049E (PlayDispContent.aar) show Wii Remote
// buttons and motions; with [game] fishing_pc_guide on they are rebuilt with keyboard / mouse glyphs
// (fishing_guide_image.h) when the game first loads them.
//
// Hook: ALTexture::LoadById(id) 803cae2c, the cache miss of ALTexture::GetById 803cafbc (vault entities/menus.md
// §8c1). Its ALIG branch is ALImage::LoadById 80345dd4 -> texture factory *(r13-0x4A7C) vtbl+0x10 -> tex vtbl+0x2C
// SetImage (GXInitTexObj) -> img vtbl+0x10 release -> tex vtbl+0x68 SetKey(id) -> ALShareList::Add(8068093C, tex).
// For these ids the wrapper does the same with an RGBA8 ('GC32') image made by ALImage::CreateByFourCc 80345bb4.
// The PlayGuide's PICT hangs from its top-right corner (SetCenterMode(2, 0)) and draws at the texture's own size
// (+0x2C / +0x2E), so a taller picture grows downward. The source pictures are 'GACC' ALIGs: a CMPR colour layer,
// then a CMPR layer whose grey is the alpha.
#include "hle_stubs.h"
#include "abi_bridge.h"
#include "fishing_guide_image.h"
#include "guest_call.h"
#include "guest_function_imports.h"
#include "gx_texel.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace {

#include "fishing_glyphs.inc"
#include "pc_art.inc"

using OpucGuest::Call;
using OpucFishing::Image;

constexpr uint32_t kLoadById = 0x803CAE2C;
constexpr uint32_t kImageLoadById = 0x80345DD4;
constexpr uint32_t kImageCreateByFourCc = 0x80345BB4;
constexpr uint32_t kShareListAdd = 0x803BA278;
constexpr uint32_t kTextureShareList = 0x8068093C;
constexpr uint32_t kTextureFactorySlot = 0x8068FEA0 - 0x4A7C;  // r13 - 0x4A7C
constexpr uint32_t kFourCcGacc = 0x43434147, kFourCcGc32 = 0x32334347;

bool GuestPtr(uint32_t p) { return (p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u); }  // MEM1, MEM2
// Virtual call targets: the texture factory keeps its vtable at +0, ALImage / ALTexture objects at +4 (after a u16
// reference count), as LoadById reads them.
uint32_t VFunc0(uint32_t obj, uint32_t off) { return Memory::Read32(Memory::Read32(obj) + off); }
uint32_t VFunc(uint32_t obj, uint32_t off) { return Memory::Read32(Memory::Read32(obj + 4) + off); }

bool IsGuide(uint32_t id) { return id >= 0xA00498 && id <= 0xA0049E; }
// A transparent 8 x 8 picture for the assist HUD (fishing_pc.cpp: the gauges' HP / SP plate); no file has this id.
constexpr uint32_t kBlankTex = 0xA0FFF0;

// The game picture `id` as straight RGBA (cached: the archive is mounted for the whole adventure).
std::map<uint32_t, Image> g_raw;
const Image* Raw(const CpuContext& c, uint32_t id) {
    auto it = g_raw.find(id);
    if (it != g_raw.end()) return it->second.Empty() ? nullptr : &it->second;
    Image& out = g_raw[id];
    const uint32_t img = Call(c, kImageLoadById, {id});
    if (!GuestPtr(img)) return nullptr;
    // ALImageGC: vtable +4, u16 width +8 / height +0xA, pixels +0xC.
    const int w = Memory::Read16(img + 8), h = Memory::Read16(img + 0xA);
    const uint32_t px = Memory::Read32(img + 0xC);
    const size_t layer = OpucGx::ByteSize(14, w, h);
    if (GuestPtr(px) && w > 0 && h > 0 && w <= 1024 && h <= 1024) {
        std::vector<uint8_t> bytes(layer * 2);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = Memory::Read8(px + static_cast<uint32_t>(i));
        const std::vector<uint8_t> color = OpucGx::Decode(14, w, h, bytes.data());
        const std::vector<uint8_t> alpha = OpucGx::Decode(14, w, h, bytes.data() + layer);
        out = Image::Make(w, h);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            std::copy_n(&color[i * 4], 3, &out.rgba[i * 4]);
            out.rgba[i * 4 + 3] = alpha[i * 4 + 1];
        }
    }
    Call(c, VFunc(img, 0x10), {img});  // release
    return out.Empty() ? nullptr : &out;
}

std::map<std::string, Image> g_glyphs;
OpucFishing::GlyphSprite Glyph(const char* name) {
    for (const GlyphRow& r : kGlyphRows) {
        if (std::string(r.name) != name) continue;
        Image& im = g_glyphs[name];
        if (im.Empty()) {
            im = Image::Make(r.w, r.h);
            std::copy_n(r.rgba, static_cast<size_t>(r.w) * r.h * 4, im.rgba.data());
        }
        return {&im, r.x, r.y};
    }
    return {nullptr, 0, 0};
}

std::map<uint32_t, Image> g_built;

// The PC picture for `id`, built once from the game's pictures.
const Image* Built(const CpuContext& c, uint32_t id) {
    auto it = g_built.find(id);
    if (it != g_built.end()) return it->second.Empty() ? nullptr : &it->second;
    auto raw = [&](uint32_t i) { return Raw(c, i); };
    static std::unique_ptr<OpucFishing::CaptionFont> font;
    if (!font) font = std::make_unique<OpucFishing::CaptionFont>(raw);
    Image& img = g_built[id];
    img = OpucFishing::BuildGuide(id, raw, *font, Glyph);
    // GX keeps RGBA8 in 4 x 4 tiles and the GC32 image buffer holds w * h texels: pad to whole tiles with
    // transparent rows (the PICT hangs from its top edge, so they add nothing visible).
    if (!img.Empty() && ((img.w | img.h) & 3)) {
        Image padded = Image::Make((img.w + 3) & ~3, (img.h + 3) & ~3);
        OpucFishing::Draw(padded, img, 0, 0, img.w, img.h, 0, 0, img.h, true);
        img = std::move(padded);
    }
    RT_LOG(RT_TAG_HLE) << "[opuc] fishing: PC PlayGuide 0x" << std::hex << id << std::dec << " " << img.w << "x" << img.h
                       << (font->Ok() ? "" : " (no caption font)") << std::endl;
    return img.Empty() ? nullptr : &img;
}

// ---- PC button art ----

const PcArtPic* FindArt(uint32_t id) {
    for (const PcArtPic& p : kPcArtPics) if (p.id == id) return &p;
    return nullptr;
}

// The recorded canvas operations on the game picture: drawImage(src, sx, sy, sw, sh, dx, dy, dw, dh) and
// clearRect(dx, dy, dw, dh) under the 2D transform m (a, b, c, d, e, f), with the canvas's sampling (bilinear while
// imageSmoothingEnabled, else nearest; sampling stays inside the source rectangle) and source-over compositing.
Image ReplayArt(const PcArtPic& p, const Image& src) {
    Image out = Image::Make(p.w, p.h);
    for (uint32_t k = 0; k < p.opCount; ++k) {
        const PcArtOp& o = p.ops[k];
        if (o.kind == 0 && o.dw == 0 && o.dh == 0) continue;  // the placeholder of a picture without operations
        const float a = o.m[0], b = o.m[1], c = o.m[2], d = o.m[3], e = o.m[4], f = o.m[5];
        const float det = a * d - b * c;
        if (det == 0) continue;
        // Device bounding box of the destination rectangle.
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        for (int i = 0; i < 4; ++i) {
            const float ux = o.dx + ((i & 1) ? o.dw : 0), uy = o.dy + ((i & 2) ? o.dh : 0);
            const float X = a * ux + c * uy + e, Y = b * ux + d * uy + f;
            x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y);
        }
        const float rx0 = std::min(o.dx, o.dx + o.dw), rx1 = std::max(o.dx, o.dx + o.dw);
        const float ry0 = std::min(o.dy, o.dy + o.dh), ry1 = std::max(o.dy, o.dy + o.dh);
        for (int py = std::max(0, static_cast<int>(std::floor(y0))); py < std::min(out.h, static_cast<int>(std::ceil(y1))); ++py) {
            for (int px = std::max(0, static_cast<int>(std::floor(x0))); px < std::min(out.w, static_cast<int>(std::ceil(x1))); ++px) {
                const float X = px + 0.5f - e, Y = py + 0.5f - f;
                const float ux = (d * X - c * Y) / det, uy = (-b * X + a * Y) / det;
                if (ux < rx0 || ux >= rx1 || uy < ry0 || uy >= ry1) continue;
                uint8_t* dst = out.At(px, py);
                if (o.kind == 1) { dst[0] = dst[1] = dst[2] = dst[3] = 0; continue; }
                const float sxf = o.sx + (ux - o.dx) * o.sw / o.dw, syf = o.sy + (uy - o.dy) * o.sh / o.dh;
                const int sxl = static_cast<int>(std::floor(std::min(o.sx, o.sx + o.sw))), sxh = static_cast<int>(std::ceil(std::max(o.sx, o.sx + o.sw))) - 1;
                const int syl = static_cast<int>(std::floor(std::min(o.sy, o.sy + o.sh))), syh = static_cast<int>(std::ceil(std::max(o.sy, o.sy + o.sh))) - 1;
                auto texel = [&](int x, int y) {
                    x = std::clamp(x, std::max(0, sxl), std::min(src.w - 1, sxh));
                    y = std::clamp(y, std::max(0, syl), std::min(src.h - 1, syh));
                    return src.At(x, y);
                };
                uint8_t s4[4];
                if (o.smooth) {
                    // Bilinear on premultiplied colour.
                    const float fx = sxf - 0.5f, fy = syf - 0.5f;
                    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
                    const float tx = fx - ix, ty = fy - iy;
                    float acc[4] = {0, 0, 0, 0};
                    for (int j = 0; j < 4; ++j) {
                        const uint8_t* t = texel(ix + (j & 1), iy + (j >> 1));
                        const float wgt = ((j & 1) ? tx : 1 - tx) * ((j >> 1) ? ty : 1 - ty), al = t[3] / 255.f;
                        acc[0] += t[0] * al * wgt; acc[1] += t[1] * al * wgt; acc[2] += t[2] * al * wgt; acc[3] += al * wgt;
                    }
                    for (int ch = 0; ch < 3; ++ch) s4[ch] = static_cast<uint8_t>(std::clamp(std::lround(acc[3] > 0 ? acc[ch] / acc[3] : 0), 0L, 255L));
                    s4[3] = static_cast<uint8_t>(std::clamp(std::lround(acc[3] * 255), 0L, 255L));
                } else {
                    std::copy_n(texel(static_cast<int>(std::floor(sxf)), static_cast<int>(std::floor(syf))), 4, s4);
                }
                OpucFishing::BlendPixel(dst, s4);
            }
        }
    }
    // The glyph layer over it.
    for (int y = 0; y < p.gh; ++y)
        for (int x = 0; x < p.gw; ++x) {
            const int tx = p.gx + x, ty = p.gy + y;
            if (tx >= 0 && ty >= 0 && tx < out.w && ty < out.h) OpucFishing::BlendPixel(out.At(tx, ty), &p.glyph[(static_cast<size_t>(y) * p.gw + x) * 4]);
        }
    return out;
}

// A 'GACC' image object (CMPR colour layer, then a CMPR layer whose grey is the alpha) as straight RGBA.
bool DecodeGacc(uint32_t img, Image& out) {
    const int w = Memory::Read16(img + 8), h = Memory::Read16(img + 0xA);
    const uint32_t px = Memory::Read32(img + 0xC);
    if (!GuestPtr(px) || w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    const size_t layer = OpucGx::ByteSize(14, w, h);
    std::vector<uint8_t> bytes(layer * 2);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = Memory::Read8(px + static_cast<uint32_t>(i));
    const std::vector<uint8_t> color = OpucGx::Decode(14, w, h, bytes.data());
    const std::vector<uint8_t> alpha = OpucGx::Decode(14, w, h, bytes.data() + layer);
    out = Image::Make(w, h);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        std::copy_n(&color[i * 4], 3, &out.rgba[i * 4]);
        out.rgba[i * 4 + 3] = alpha[i * 4 + 1];
    }
    return true;
}

std::map<uint32_t, Image> g_art;
const Image* BuiltArt(const PcArtPic& p, const Image* src) {
    auto it = g_art.find(p.id);
    if (it != g_art.end()) return it->second.Empty() ? nullptr : &it->second;
    Image& img = g_art[p.id];
    if (src && src->w == p.srcW && src->h == p.srcH) img = ReplayArt(p, *src);
    RT_LOG(RT_TAG_HLE) << "[opuc] pc art: 0x" << std::hex << p.id << std::dec << " (" << p.file << ") "
                       << (img.Empty() ? "kept the game's picture (size differs)" : std::to_string(img.w) + "x" + std::to_string(img.h)) << std::endl;
    return img.Empty() ? nullptr : &img;
}

// A GC32 image of pic's size from the texture factory's handler: allocated in whole 4 x 4 tiles, then given the
// picture's true size, so SetImage builds a texture of exactly that size (SetupTexObj takes the image's width and
// height; GX lays the tiles out from the width rounded up to 4).
uint32_t MakeGc32(const CpuContext& c, const Image& pic) {
    const int pw = (pic.w + 3) & ~3, ph = (pic.h + 3) & ~3;
    const uint32_t img = Call(c, kImageCreateByFourCc, {kFourCcGc32, static_cast<uint32_t>(pw), static_cast<uint32_t>(ph)});
    const uint32_t px = GuestPtr(img) ? Memory::Read32(img + 0xC) : 0;
    if (!GuestPtr(px)) {
        if (GuestPtr(img)) Call(c, VFunc(img, 0x10), {img});
        return 0;
    }
    const std::vector<uint8_t> tex = OpucGx::EncodeRgba8(pic.w, pic.h, pic.rgba.data());
    if (tex.size() != static_cast<size_t>(pw) * ph * 4) { Call(c, VFunc(img, 0x10), {img}); return 0; }
    for (size_t i = 0; i < tex.size(); ++i) Memory::Write8(px + static_cast<uint32_t>(i), tex[i]);
    Memory::Write16(img + 8, static_cast<uint16_t>(pic.w));
    Memory::Write16(img + 0xA, static_cast<uint16_t>(pic.h));
    Memory::Write8(img + 0x12, 1);  // has alpha
    return img;
}

constexpr uint32_t kGaccVtable = 0x805D63A8;  // ALImageHandlerGC::CreateByFourCc's vtable for 'GACC' images

// An ALTX check bubble: the game built its atlas texture (frame table, centre pivot); give it the PC image.
void PatchAtlas(const CpuContext& c, uint32_t tex, const PcArtPic& p) {
    const uint32_t img = GuestPtr(tex) ? Memory::Read32(tex + 0x20) : 0;
    Image src;
    if (!GuestPtr(img) || Memory::Read32(img + 4) != kGaccVtable || !DecodeGacc(img, src)) {
        RT_LOG(RT_TAG_HLE) << "[opuc] pc art: 0x" << std::hex << p.id << " atlas texture 0x" << tex << " image 0x" << img
                           << " vtable 0x" << (GuestPtr(img) ? Memory::Read32(img + 4) : 0) << std::dec << ": not a GACC image, kept" << std::endl;
        return;
    }
    const Image* pic = BuiltArt(p, &src);
    if (!pic) return;
    const uint32_t gc = MakeGc32(c, *pic);
    if (!gc) return;
    Memory::Write8(tex + 0x3E, 1);  // one layer (SetupTexObj only ever sets 2, for the two-layer formats)
    Call(c, VFunc(tex, 0x2C), {tex, gc});
    Call(c, VFunc(gc, 0x10), {gc});
}

// ---- Tutorial texts (pcart.js TEXT) ----
bool g_inTutorialLayout = false;

// WinComText row 2 "[icon:6] [icon:6] (Hold): Dash": C C held is one held dodge key; row 4 "[icon:7]: Dodge": A + B
// is the same key tapped.
void SwapTutorialText(uint32_t at) {
    std::u16string s = OpucGuest::ReadWide(at, 255);
    const std::u16string before = s;
    auto replace = [&](std::u16string_view from, std::u16string_view to) {
        const size_t i = s.find(from);
        if (i != std::u16string::npos) s.replace(i, from.size(), to);
    };
    replace(u"[icon:6] [icon:6] (Hold)", u"[icon:6] (Hold)");
    replace(u"[icon:7]:", u"[icon:7] (Tap):");
    if (s == before) return;
    OpucGuest::WriteWide(at, s.substr(0, 254));
    RT_LOG(RT_TAG_HLE) << "[opuc] pc art: tutorial text reworded for the PC keys" << std::endl;
}

}  // namespace

// AquaDesignerTutorialWindow::LayoutText 8015bbe0 copies its WinComText row with wstr::Copy 802a6068 before it
// parses the [icon:k] tags; the copy is reworded while LayoutText runs.
extern "C" void OpucPcArt_TutorialLayoutText(CpuContext* ctx) {
    g_inTutorialLayout = RuntimeConfigFile::PcButtonArt(true);
    GuestFunctions::NextTranslation(0x8015BBE0)(ctx);
    g_inTutorialLayout = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x8015BBE0, OpucPcArt_TutorialLayoutText, "OpucPcArt_TutorialLayoutText");

extern "C" void OpucPcArt_WstrCopy(CpuContext* ctx) {
    const uint32_t dst = ctx->gpr[3];
    GuestFunctions::NextTranslation(0x802A6068)(ctx);
    if (g_inTutorialLayout && GuestPtr(dst)) {
        g_inTutorialLayout = false;  // the row copy is the first one
        try { SwapTutorialText(dst); } catch (...) {}
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x802A6068, OpucPcArt_WstrCopy, "OpucPcArt_WstrCopy");

extern "C" void OpucFishing_TextureLoadById(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[3];
    if (const PcArtPic* art = RuntimeConfigFile::PcButtonArt(true) ? FindArt(id) : nullptr) {
        const CpuContext c = *ctx;
        if (id >= 0xA00490 && id <= 0xA00495) {  // the ALTX check bubbles: the game's texture, a new image
            GuestFunctions::NextTranslation(kLoadById)(ctx);
            try { PatchAtlas(c, ctx->gpr[3], *art); } catch (...) {}
            return;
        }
        const Image* pic = nullptr;
        try { pic = BuiltArt(*art, Raw(c, id)); } catch (...) { pic = nullptr; }
        const uint32_t factory = GuestPtr(Memory::Read32(kTextureFactorySlot)) ? Memory::Read32(kTextureFactorySlot) : 0;
        const uint32_t gc = pic && factory ? MakeGc32(c, *pic) : 0;
        if (!gc) {
            GuestFunctions::NextTranslation(kLoadById)(ctx);
            return;
        }
        const uint32_t texObj = Call(c, VFunc0(factory, 0x10), {factory});
        Call(c, VFunc(texObj, 0x2C), {texObj, gc});
        Call(c, VFunc(gc, 0x10), {gc});
        Call(c, VFunc(texObj, 0x68), {texObj, id});
        Call(c, kShareListAdd, {kTextureShareList, texObj});
        ctx->gpr[3] = texObj;
        return;
    }
    const bool blank = id == kBlankTex;
    if (!blank && (!IsGuide(id) || !RuntimeConfigFile::FishingPcGuide(true))) {
        GuestFunctions::NextTranslation(kLoadById)(ctx);
        return;
    }
    const CpuContext c = *ctx;
    const Image* pic = nullptr;
    static Image blankImage = Image::Make(8, 8);
    try { pic = blank ? &blankImage : Built(c, id); } catch (...) { pic = nullptr; }
    const uint32_t factory = GuestPtr(Memory::Read32(kTextureFactorySlot)) ? Memory::Read32(kTextureFactorySlot) : 0;
    const uint32_t img = pic && factory ? Call(c, kImageCreateByFourCc, {kFourCcGc32, static_cast<uint32_t>(pic->w), static_cast<uint32_t>(pic->h)}) : 0;
    const uint32_t px = GuestPtr(img) ? Memory::Read32(img + 0xC) : 0;
    if (!GuestPtr(px)) {
        if (GuestPtr(img)) Call(c, VFunc(img, 0x10), {img});
        GuestFunctions::NextTranslation(kLoadById)(ctx);
        return;
    }
    const std::vector<uint8_t> tex = OpucGx::EncodeRgba8(pic->w, pic->h, pic->rgba.data());
    if (tex.size() != static_cast<size_t>(pic->w) * pic->h * 4) {  // never write past the w * h * 4 buffer
        Call(c, VFunc(img, 0x10), {img});
        GuestFunctions::NextTranslation(kLoadById)(ctx);
        return;
    }
    for (size_t i = 0; i < tex.size(); ++i) Memory::Write8(px + static_cast<uint32_t>(i), tex[i]);
    Memory::Write8(img + 0x12, 1);  // has alpha
    const uint32_t texObj = Call(c, VFunc0(factory, 0x10), {factory});
    Call(c, VFunc(texObj, 0x2C), {texObj, img});  // SetImage: GXInitTexObj from the image
    Call(c, VFunc(img, 0x10), {img});             // release the image
    Call(c, VFunc(texObj, 0x68), {texObj, id});   // SetKey
    Call(c, kShareListAdd, {kTextureShareList, texObj});
    ctx->gpr[3] = texObj;
}
REGISTER_NATIVE_WRAPPER_AS(0x803CAE2C, OpucFishing_TextureLoadById, "OpucFishing_TextureLoadById");
