// lut_hdr_vlc.cpp
//
// VLC video filter plugin wrapping the existing LUT/HDR color-correction engine.
//
// DELIBERATE ARCHITECTURE CHOICE: this does NOT hook into VLC's D3D11 opaque
// hardware-surface filter chain (modules/hw/d3d11/*). That layer's internal
// structures (d3d11_video_context_t, picture_sys_d3d11_t, etc.) change across
// VLC versions and I could not verify their current exact shape against your
// installed VLC's headers from here. Instead this is a plain "video filter"
// module operating on ordinary CPU-accessible picture_t planes - the most
// stable, long-unchanged part of VLC's filter API. Compute still runs on the
// GPU: D3D11LutEngine::ProcessFrameP010 already does its own upload -> compute
// shader -> staging-texture readback against plain system-memory pointers, so
// nothing about the actual color processing changes. The only difference from
// a fully opaque zero-copy path is one extra CPU<->GPU copy per frame, which is
// insignificant next to the compute shader work itself.
//
// KNOWN GAP: D3D11LutEngine currently only implements a P010 (10-bit) path.
// 8-bit NV12 is routed through the existing CPU implementation in YuvOps.cpp
// (ProcessFrameNV12) instead of being silently dropped - slower, but correct,
// and it's the same code path used before any GPU engine existed at all.
//
// BUILD NOTE: this file, plus D3D11LutEngine.cpp/h, Lut3D.cpp/h, YuvOps.cpp/h,
// PixelOps.cpp (for ApplyToneMapAndLut used by the NV12 CPU path... actually
// NV12 here goes through ProcessFrameNV12 directly, see below), and ColorSpace.h
// need to be compiled together into one plugin DLL, linked against libvlccore,
// d3d11.lib, d3dcompiler.lib. See the CMakeLists.txt written alongside this file.

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_atomic.h>

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <vector>
#include <string>
#include <windows.h>

#include "D3D11LutEngine.h"
#include "Lut3D.h"
#include "ColorSpace.h"
#include "YuvOps.h"
#include "PixelOps.h"

namespace
{
    void LutHdrVlcTrace(vlc_object_t* obj, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        msg_Dbg(obj, "LUTHDR: %s", buf);
    }

    // --- VLC <-> internal color-info mapping -------------------------------
    // VLC's video_format_t carries the same kind of primaries/transfer/matrix/
    // range signaling as DirectShow's VIDEOINFOHEADER2, just under different
    // enum names (video_color_*.h). Mapped here rather than duplicating
    // ColorSpace.h's enums, so D3D11LutEngine/YuvOps/Lut3D stay untouched and
    // shared verbatim with the DirectShow filter.
    DetectedColorInfo MapVlcColorInfo(const video_format_t* fmt)
    {
        DetectedColorInfo info;

        // VLC reports COLOR_PRIMARIES_UNDEF/TRANSFER_FUNC_UNDEF/etc. when the
        // stream didn't signal anything, same idea as AMCONTROL_COLORINFO_PRESENT
        // being unset - treat that as "not present" rather than guessing.
        const bool anySignaled =
            fmt->primaries != COLOR_PRIMARIES_UNDEF ||
            fmt->transfer != TRANSFER_FUNC_UNDEF ||
            fmt->space != COLOR_SPACE_UNDEF ||
            fmt->color_range != COLOR_RANGE_UNDEF;

        if (!anySignaled)
            return info; // present=false, everything Unknown - matches default DetectedColorInfo

        info.present = true;

        switch (fmt->primaries)
        {
        case COLOR_PRIMARIES_BT709:     info.space = SourceColorSpace::BT709;  break;
        case COLOR_PRIMARIES_BT2020:    info.space = SourceColorSpace::BT2020; break;
        case COLOR_PRIMARIES_DCI_P3:    info.space = SourceColorSpace::DCI_P3; break;
        default:                        info.space = SourceColorSpace::Unknown; break;
        }

        switch (fmt->transfer)
        {
        case TRANSFER_FUNC_SMPTE_ST2084: info.transfer = SourceTransfer::PQ;        break;
        case TRANSFER_FUNC_HLG:          info.transfer = SourceTransfer::HLG;       break;
        case TRANSFER_FUNC_SRGB:
        case TRANSFER_FUNC_BT709:        info.transfer = SourceTransfer::SDR_Gamma; break;
        default:                         info.transfer = SourceTransfer::Unknown;   break;
        }

        switch (fmt->space)
        {
        case COLOR_SPACE_BT601:  info.matrix = SourceMatrix::BT601;  break;
        case COLOR_SPACE_BT709:  info.matrix = SourceMatrix::BT709;  break;
        case COLOR_SPACE_BT2020: info.matrix = SourceMatrix::BT2020; break;
        default:                 info.matrix = SourceMatrix::Unknown; break;
        }

        switch (fmt->color_range)
        {
        case COLOR_RANGE_FULL:    info.range = SourceRange::Full;    break;
        case COLOR_RANGE_LIMITED: info.range = SourceRange::Limited; break;
        default:                  info.range = SourceRange::Unknown; break;
        }

        return info;
    }
}

// --- Filter instance state --------------------------------------------------

struct filter_sys_t
{
    D3D11LutEngine gpuEngine;

    std::wstring pathLut709;
    std::wstring pathLut2020;
    std::wstring pathLutDCIP3;

    bool forceP010AsHlg = false;
    bool applyHlgOotf = false;

    bool lut709Loaded = false;
    bool lut2020Loaded = false;
    bool lutDCIP3Loaded = false;

    ToneMapSettings toneMap; // used only by the CPU NV12 fallback path
    Lut3D cpuLut709, cpuLut2020, cpuLutDciP3; // CPU-side copies for the NV12 path,
                                               // loaded from the same files as the GPU LUTs
};

namespace
{
    bool ShouldProcess(const DetectedColorInfo& info, bool forceP010AsHlg, bool isP010, int width)
    {
        SourceTransfer transfer = info.transfer;

        // Same conservative default as the DirectShow filter: never guess HDR from
        // resolution/pixel-format alone. Only the explicit opt-in below does that,
        // and it's off unless the user set it.
        if (!info.present && isP010 && width >= 3840 && forceP010AsHlg)
            transfer = SourceTransfer::HLG;

        return transfer == SourceTransfer::HLG;
    }

    void EnsureLutsLoaded(filter_sys_t* sys, vlc_object_t* obj)
    {
        auto loadBoth = [&](const std::wstring& path, bool& gpuFlag, Lut3D& cpuLut, SourceColorSpace space)
        {
            if (path.empty())
                return;
            if (!gpuFlag)
            {
                gpuFlag = sys->gpuEngine.LoadLutDirect(space, path);
                LutHdrVlcTrace(obj, "GPU LUT load %s: %s", gpuFlag ? "OK" : "FAILED",
                               std::string(path.begin(), path.end()).c_str());
            }
            if (!cpuLut.IsLoaded())
                cpuLut.LoadFile(path); // used only by the NV12 CPU fallback path
        };

        loadBoth(sys->pathLut709, sys->lut709Loaded, sys->cpuLut709, SourceColorSpace::BT709);
        loadBoth(sys->pathLut2020, sys->lut2020Loaded, sys->cpuLut2020, SourceColorSpace::BT2020);
        loadBoth(sys->pathLutDCIP3, sys->lutDCIP3Loaded, sys->cpuLutDciP3, SourceColorSpace::DCI_P3);
    }

    const Lut3D& SelectCpuLut(filter_sys_t* sys, SourceColorSpace space)
    {
        switch (space)
        {
        case SourceColorSpace::BT2020: return sys->cpuLut2020;
        case SourceColorSpace::DCI_P3: return sys->cpuLutDciP3;
        case SourceColorSpace::BT709:
        default:                       return sys->cpuLut709;
        }
    }
}

// --- Main per-frame callback -------------------------------------------------

static picture_t* Filter(filter_t* filter, picture_t* pic)
{
    filter_sys_t* sys = reinterpret_cast<filter_sys_t*>(filter->p_sys);

    const video_format_t* fmt = &pic->format;
    const int width = fmt->i_visible_width;
    const int height = fmt->i_visible_height;

    const bool isP010 = (fmt->i_chroma == VLC_CODEC_P010);
    const bool isNV12 = (fmt->i_chroma == VLC_CODEC_NV12);

    if (!isP010 && !isNV12)
    {
        // Not a chroma we handle at all (e.g. RGB, I420) - straight passthrough.
        // (A GPU RGB32 path and an 8-bit-direct GPU path both remain future work;
        // see the DirectShow filter's PixelOps.cpp ProcessFrameRGB32 for the
        // equivalent CPU logic if that's ever needed here too.)
        return pic;
    }

    DetectedColorInfo colorInfo = MapVlcColorInfo(fmt);

    if (!colorInfo.present && isP010 && width >= 3840 && width == static_cast<int>(fmt->i_visible_width))
    {
        // Mirrors the (opt-in only) fallback in the DirectShow filter's
        // SetMediaType - see ShouldProcess() below for the actual gating logic.
    }

    if (!ShouldProcess(colorInfo, sys->forceP010AsHlg, isP010, width))
        return pic; // bypass: not HLG-signaled content, leave untouched

    EnsureLutsLoaded(sys, VLC_OBJECT(filter));

    picture_t* out = filter_NewPicture(filter);
    if (!out)
        return pic; // allocation failed - fail safe to passthrough rather than drop the frame

    if (isP010)
    {
        sys->gpuEngine.SetApplyHlgOotf(sys->applyHlgOotf);

        const uint8_t* srcY = pic->p[0].p_pixels;
        const uint8_t* srcUV = pic->p[1].p_pixels;
        uint8_t* dstY = out->p[0].p_pixels;
        uint8_t* dstUV = out->p[1].p_pixels;

        const int srcStride = pic->p[0].i_pitch;
        const int dstStride = out->p[0].i_pitch;

        // ProcessFrameP010 wants ONE contiguous src/dst buffer with the UV plane
        // living srcCodedHeight/dstCodedHeight rows below the Y plane (that's how
        // the DirectShow media-sample buffer it was originally written for is laid
        // out). VLC hands us two independent plane pointers instead, which may not
        // be contiguous in memory at all. Cheapest correct fix: stage each side
        // into one contiguous scratch buffer matching that expected layout, then
        // scatter the result back out to the two real planes afterward.
        //
        // TODO(perf): if profiling shows this copy matters, extend
        // D3D11LutEngine::ProcessFrameP010 to take independent Y/UV src and dst
        // pointers directly (it already tracks Y/UV as separate GPU textures
        // internally - UpdateSubresource/readback just need two calls per side
        // instead of one) and delete this staging step entirely.
        const int uvHeight = (height + 1) / 2;
        const size_t srcTotal = static_cast<size_t>(srcStride) * height + static_cast<size_t>(srcStride) * uvHeight;
        const size_t dstTotal = static_cast<size_t>(dstStride) * height + static_cast<size_t>(dstStride) * uvHeight;

        std::vector<uint8_t> srcScratch(srcTotal);
        std::vector<uint8_t> dstScratch(dstTotal);

        memcpy(srcScratch.data(), srcY, static_cast<size_t>(srcStride) * height);
        memcpy(srcScratch.data() + static_cast<size_t>(srcStride) * height, srcUV,
               static_cast<size_t>(srcStride) * uvHeight);

        const bool ok = sys->gpuEngine.ProcessFrameP010(
            srcScratch.data(), dstScratch.data(),
            width, height,
            srcStride, height,   // srcCodedHeight == height: our scratch buffer is never padded
            dstStride, height,   // same for dst
            colorInfo);

        if (ok)
        {
            memcpy(dstY, dstScratch.data(), static_cast<size_t>(dstStride) * height);
            memcpy(dstUV, dstScratch.data() + static_cast<size_t>(dstStride) * height,
                   static_cast<size_t>(dstStride) * uvHeight);
        }
        else
        {
            LutHdrVlcTrace(VLC_OBJECT(filter), "GPU ProcessFrameP010 failed, passthrough copy");
            memcpy(dstY, srcY, static_cast<size_t>(srcStride) * height);
            memcpy(dstUV, srcUV, static_cast<size_t>(srcStride) * uvHeight);
        }
    }
    else // isNV12 - CPU path, see file header
    {
        // ProcessFrameNV12 works in place, so copy source into the destination
        // picture first, then run the transform on the copy.
        memcpy(out->p[0].p_pixels, pic->p[0].p_pixels,
               static_cast<size_t>(pic->p[0].i_pitch) * pic->p[0].i_lines);
        memcpy(out->p[1].p_pixels, pic->p[1].p_pixels,
               static_cast<size_t>(pic->p[1].i_pitch) * pic->p[1].i_lines);

        const Lut3D& lut = SelectCpuLut(sys, colorInfo.space);
        ProcessFrameNV12(
            out->p[0].p_pixels, out->p[1].p_pixels,
            width, height,
            out->p[0].i_pitch, out->p[1].i_pitch,
            colorInfo, sys->toneMap, lut);
    }

    picture_CopyProperties(out, pic);
    picture_Release(pic);
    return out;
}

// --- Module options (vlc.exe --lut-hdr-lut2020=... / vlcrc / GUI prefs) ---

#define LUT709_TEXT     N_("BT.709 LUT file")
#define LUT2020_TEXT    N_("BT.2020 LUT file")
#define LUTDCIP3_TEXT   N_("DCI-P3 LUT file")
#define FORCE_HLG_TEXT  N_("Force unflagged 4K P010 as HLG")
#define FORCE_HLG_LONG  N_("Treat P010 4K video with no signaled color info as BT.2020 HLG. " \
                            "Off by default - resolution/pixel-format alone does not reliably indicate HDR.")
#define OOTF_TEXT       N_("Apply HLG EOTF+OOTF before the LUT")
#define OOTF_LONG       N_("Decode HLG (inverse OETF + BT.2100 system-gamma OOTF) before handing RGB " \
                            "to the 3D LUT, instead of feeding the LUT the raw signal directly. " \
                            "Whether this is correct depends on how your specific LUT files were authored - test both.")

static int Open(vlc_object_t* obj);
static void Close(filter_t* filter);

vlc_module_begin()
    set_shortname(N_("LUT/HDR Color Filter"))
    set_description(N_("3D LUT + HDR (HLG) tone-mapping video filter"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_shortcut("lut_hdr")
    set_callbacks(Open, Close)

    add_loadfile("lut-hdr-lut709", nullptr, LUT709_TEXT, nullptr)
    add_loadfile("lut-hdr-lut2020", nullptr, LUT2020_TEXT, nullptr)
    add_loadfile("lut-hdr-lutdcip3", nullptr, LUTDCIP3_TEXT, nullptr)
    add_bool("lut-hdr-force-p010-hlg", false, FORCE_HLG_TEXT, FORCE_HLG_LONG)
    add_bool("lut-hdr-apply-hlg-ootf", false, OOTF_TEXT, OOTF_LONG)
vlc_module_end()

namespace
{
    std::wstring Utf8ToWide(const char* s)
    {
        if (!s) return L"";
        std::wstring out;
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (len <= 0) return L"";
        out.resize(len - 1);
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
        return out;
    }
}

static int Open(vlc_object_t* obj)
{
    filter_t* filter = reinterpret_cast<filter_t*>(obj);

    if (filter->fmt_in.video.i_chroma != VLC_CODEC_P010 &&
        filter->fmt_in.video.i_chroma != VLC_CODEC_NV12)
        return VLC_EGENERIC; // let VLC skip this filter for chromas it can't handle

    auto* sys = new (std::nothrow) filter_sys_t();
    if (!sys)
        return VLC_ENOMEM;

    if (!sys->gpuEngine.Initialize())
    {
        msg_Err(obj, "LUTHDR: D3D11 device initialization failed");
        delete sys;
        return VLC_EGENERIC;
    }

    char* p709 = var_InheritString(obj, "lut-hdr-lut709");
    char* p2020 = var_InheritString(obj, "lut-hdr-lut2020");
    char* pDciP3 = var_InheritString(obj, "lut-hdr-lutdcip3");
    sys->pathLut709 = Utf8ToWide(p709);
    sys->pathLut2020 = Utf8ToWide(p2020);
    sys->pathLutDCIP3 = Utf8ToWide(pDciP3);
    free(p709); free(p2020); free(pDciP3);

    sys->forceP010AsHlg = var_InheritBool(obj, "lut-hdr-force-p010-hlg");
    sys->applyHlgOotf = var_InheritBool(obj, "lut-hdr-apply-hlg-ootf");

    LutHdrVlcTrace(obj, "Open: gpu=OK force709=%d 2020=%d dcip3=%d ForceP010AsHLG=%d ApplyHlgOOTF=%d",
                   !sys->pathLut709.empty(), !sys->pathLut2020.empty(), !sys->pathLutDCIP3.empty(),
                   sys->forceP010AsHlg, sys->applyHlgOotf);

    filter->p_sys = reinterpret_cast<filter_sys_t*>(sys);
    filter->pf_video_filter = Filter;
    return VLC_SUCCESS;
}

static void Close(filter_t* filter)
{
    delete reinterpret_cast<filter_sys_t*>(filter->p_sys);
}
