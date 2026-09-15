// lut_hdr_vlc.cpp
//
// VLC video filter plugin wrapping the existing LUT/HDR color-correction engine.
// This is the VLC counterpart of LutHdrFilter.cpp (the MPC-BE/DirectShow build) -
// same Lut3D/PixelOps/YuvOps/D3D11LutEngine underneath, different host glue.
//
// ARCHITECTURE, same as the DirectShow build: plain CPU-accessible picture_t
// planes, not VLC's opaque D3D11 hardware-surface chain (modules/hw/d3d11/*).
// D3D11LutEngine::ProcessFrameP010 does its own upload -> compute shader ->
// staging-texture readback against system-memory pointers, so this only costs
// one extra CPU<->GPU copy per frame versus a fully opaque path.
//
// WHAT DIDN'T PORT 1:1 FROM THE DIRECTSHOW BUILD, AND WHY:
//
//  - Property page (LutHdrPropPage.*) -> module config options (add_string /
//    add_bool below). VLC generates the preferences UI from these automatically
//    ("Video filters" section); there's no VLC equivalent of a COM property page
//    to port, and none is needed.
//  - HotkeyManager's OS-level RegisterHotKey combos -> two *live* module
//    variables instead (`lut-hdr-enabled`, `lut-hdr-force-space`). VLC modules
//    can't cleanly register arbitrary global hotkeys the way a DirectShow filter
//    DLL can; these vars are toggleable at runtime from the "Adjustments and
//    Effects" dialog (or `--lut-hdr-enabled=0`, or a Lua/telnet interface) which
//    covers the same *capability* (flip processing / force a LUT mid-playback)
//    without a fifth of a wall of RegisterHotKey combos to manage.
//  - DecideBufferSize's sample-size-driven stride/coded-height guessing -> not
//    needed. VLC's filter_NewPicture() gives us an output picture_t whose
//    per-plane i_pitch/i_lines are authoritative, and the input picture_t is the
//    same for its own planes - no "which allocator padded what" guessing game.
//  - FourCCSubtype.h's GUID-pattern FourCC compare -> plain vlc_fourcc_t (i_chroma)
//    comparisons; VLC's chroma tags already do what that header worked around.

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include <cstring>
#include <string>
#include <atomic>
#include <algorithm>
#include <windows.h>

#include "D3D11LutEngine.h"
#include "Lut3D.h"
#include "ColorSpace.h"
#include "ColorSpaceVLC.h"
#include "YuvOps.h"
#include "PixelOps.h"

#define CFG_PREFIX "lut-hdr-"

namespace
{
    std::wstring Utf8PathToWide(const char* utf8)
    {
        if (!utf8 || !*utf8)
            return std::wstring();
        int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (need <= 1)
            return std::wstring();
        std::wstring wide(static_cast<size_t>(need) - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &wide[0], need);
        return wide;
    }

    // Same decision function as the DirectShow build's ComputeShouldProcess, just
    // taking std::string LUT paths instead of std::wstring registry reads - the
    // logic (only ever process signaled-HLG content, and only if a matching LUT
    // path is actually configured) is unchanged.
    bool ComputeShouldProcess(DetectedColorInfo info, vlc_fourcc_t chroma, int width,
                               bool forceP010AsHlg,
                               const std::string& pathLut709,
                               const std::string& pathLut2020,
                               const std::string& pathLutDCIP3)
    {
        if (!info.present && chroma == VLC_CODEC_P010 && width >= 3840 && forceP010AsHlg)
        {
            info.space = SourceColorSpace::BT2020;
            info.transfer = SourceTransfer::HLG;
            info.present = true;
        }

        if (info.transfer != SourceTransfer::HLG)
            return false;

        if (info.space == SourceColorSpace::BT2020) return !pathLut2020.empty();
        if (info.space == SourceColorSpace::DCI_P3) return !pathLutDCIP3.empty();
        if (info.space == SourceColorSpace::BT709)  return !pathLut709.empty();
        return false;
    }

    struct filter_sys_t
    {
        Lut3D lut709, lut2020, lutDciP3;
        D3D11LutEngine gpuEngine;
        ToneMapSettings toneMap;

        std::string pathLut709, pathLut2020, pathLutDciP3;
        bool forceP010AsHlg = false;
        bool outputTransferFunc709 = false;
        bool applyHlgOotf = false;

        // Runtime-toggleable, replacing the DirectShow build's OS hotkeys - see
        // the file header comment. Read from the filter (video output) thread,
        // written from var callbacks fired on whatever thread the UI/telnet/Lua
        // interface runs on, hence atomic rather than plain bool/int.
        std::atomic<bool> enabled{true};
        std::atomic<int>  lutOverride{-1}; // -1=Auto, 0=BT709, 1=BT2020, 2=DCI_P3

        bool gpuTried = false; // Initialize() attempted at least once (success or fail)
    };

    void EnsureLutLoaded(filter_sys_t* sys)
    {
        // Deferred exactly like EnsureLutLoaded() in the DirectShow build: only
        // called once we already know this frame will actually be processed, so
        // ordinary SDR playback through this filter never pays for a D3D11 device
        // + compute-shader compile it will never use.
        if (!sys->gpuTried)
        {
            sys->gpuTried = true;
            sys->gpuEngine.Initialize();
        }
        if (!sys->gpuEngine.IsInitialized())
            return;

        if (!sys->pathLut709.empty())
            sys->gpuEngine.LoadLutDirect(SourceColorSpace::BT709, Utf8PathToWide(sys->pathLut709.c_str()));
        if (!sys->pathLut2020.empty())
            sys->gpuEngine.LoadLutDirect(SourceColorSpace::BT2020, Utf8PathToWide(sys->pathLut2020.c_str()));
        if (!sys->pathLutDciP3.empty())
            sys->gpuEngine.LoadLutDirect(SourceColorSpace::DCI_P3, Utf8PathToWide(sys->pathLutDciP3.c_str()));
    }

    const Lut3D& SelectCpuLut(filter_sys_t* sys, SourceColorSpace space)
    {
        switch (space)
        {
        case SourceColorSpace::BT2020: return sys->lut2020;
        case SourceColorSpace::DCI_P3: return sys->lutDciP3;
        case SourceColorSpace::BT709:
        default:                       return sys->lut709;
        }
    }

    // Plain row-by-row plane copy, honoring each picture_t's OWN pitch (VLC gives
    // us this directly via p_pic->p[i].i_pitch/i_lines - no stride-guessing needed,
    // unlike the DirectShow build's DeriveNV12P010Layout/CopyFrameRespectingStride).
    void CopyPicturePlanes(picture_t* dst, const picture_t* src)
    {
        for (int i = 0; i < src->i_planes && i < dst->i_planes; ++i)
        {
            const int rowBytes = std::min(src->p[i].i_pitch, dst->p[i].i_pitch);
            const int lines = std::min(src->p[i].i_lines, dst->p[i].i_lines);
            for (int y = 0; y < lines; ++y)
            {
                memcpy(dst->p[i].p_pixels + static_cast<size_t>(y) * dst->p[i].i_pitch,
                       src->p[i].p_pixels + static_cast<size_t>(y) * src->p[i].i_pitch,
                       rowBytes);
            }
        }
    }

    picture_t* Filter(filter_t* p_filter, picture_t* p_pic)
    {
        auto* sys = reinterpret_cast<filter_sys_t*>(p_filter->p_sys);

        picture_t* p_outpic = filter_NewPicture(p_filter);
        if (!p_outpic)
        {
            picture_Release(p_pic);
            return nullptr;
        }
        picture_CopyProperties(p_outpic, p_pic);

        const vlc_fourcc_t chroma = p_filter->fmt_in.video.i_chroma;
        const int width = p_pic->format.i_visible_width;
        const int height = p_pic->format.i_visible_height;

        DetectedColorInfo colorInfo = DecodeColorInfo(&p_filter->fmt_in.video);

        // Same opt-in "unflagged 4K P010 might be unlabeled HLG" override as the
        // DirectShow build's SetMediaType - off by default, see the module option
        // description below for how to enable it.
        if (!colorInfo.present && chroma == VLC_CODEC_P010 && width >= 3840 && sys->forceP010AsHlg)
        {
            colorInfo.space = SourceColorSpace::BT2020;
            colorInfo.matrix = SourceMatrix::BT2020;
            colorInfo.transfer = SourceTransfer::HLG;
            colorInfo.range = SourceRange::Limited;
            colorInfo.present = true;
        }

        const bool userEnabled = sys->enabled.load(std::memory_order_relaxed);
        const bool willProcess = userEnabled &&
            ComputeShouldProcess(colorInfo, chroma, width, sys->forceP010AsHlg,
                                  sys->pathLut709, sys->pathLut2020, sys->pathLutDciP3);

        if (!willProcess)
        {
            CopyPicturePlanes(p_outpic, p_pic);
            picture_Release(p_pic);
            return p_outpic;
        }

        EnsureLutLoaded(sys);

        // Live LUT-space override, replacing the DirectShow build's four
        // force-709/2020/dcip3/auto hotkeys with one variable.
        DetectedColorInfo effectiveColorInfo = colorInfo;
        switch (sys->lutOverride.load(std::memory_order_relaxed))
        {
        case 0: effectiveColorInfo.space = SourceColorSpace::BT709;  break;
        case 1: effectiveColorInfo.space = SourceColorSpace::BT2020; break;
        case 2: effectiveColorInfo.space = SourceColorSpace::DCI_P3; break;
        default: break; // -1 = Auto, keep detected space
        }

        if (chroma == VLC_CODEC_P010)
        {
            // GPU path first: operates directly src picture_t -> dst picture_t,
            // no pre-copy needed. codedHeight == i_lines for a plain CPU picture -
            // VLC doesn't hand this filter an opaque padded decode surface, unlike
            // the DirectShow build's MPC Video Renderer case.
            const bool gpuOk = sys->gpuEngine.ProcessFrameP010(
                p_pic->p[0].p_pixels, p_outpic->p[0].p_pixels,
                width, height,
                p_pic->p[0].i_pitch, p_pic->p[0].i_lines,
                p_outpic->p[0].i_pitch, p_outpic->p[0].i_lines,
                effectiveColorInfo);

            if (gpuOk)
            {
                // D3D11LutEngine only writes the Y/UV luma-chroma output it computed;
                // still copy chroma plane pointers/metadata sanity (both planes are
                // processed by ProcessFrameP010 internally) - nothing further to do.
                picture_Release(p_pic);
                return p_outpic;
            }

            // GPU unavailable/failed: fall back to the same CPU path NV12 always
            // uses, on P010 samples (matches the DirectShow build's "8-bit NV12 is
            // routed through YuvOps.cpp; P010 only has one path currently" gap -
            // except here we additionally use it as P010's fallback, which is
            // strictly more correct behavior than silently passing HDR through).
            CopyPicturePlanes(p_outpic, p_pic);
            ProcessFrameP010(
                reinterpret_cast<uint16_t*>(p_outpic->p[0].p_pixels),
                reinterpret_cast<uint16_t*>(p_outpic->p[1].p_pixels),
                width, height,
                p_outpic->p[0].i_pitch, p_outpic->p[1].i_pitch,
                effectiveColorInfo, sys->toneMap, SelectCpuLut(sys, effectiveColorInfo.space));
            picture_Release(p_pic);
            return p_outpic;
        }

        if (chroma == VLC_CODEC_NV12)
        {
            CopyPicturePlanes(p_outpic, p_pic);
            ProcessFrameNV12(
                p_outpic->p[0].p_pixels, p_outpic->p[1].p_pixels,
                width, height,
                p_outpic->p[0].i_pitch, p_outpic->p[1].i_pitch,
                effectiveColorInfo, sys->toneMap, SelectCpuLut(sys, effectiveColorInfo.space));
            picture_Release(p_pic);
            return p_outpic;
        }

        if (chroma == VLC_CODEC_RGB32)
        {
            // ASSUMPTION carried over from the DirectShow build: ProcessFrameRGB32
            // assumes BGRA byte order in memory. VLC_CODEC_RGB32's actual byte
            // order is mask-driven (p_filter->fmt_in.video.i_[rgb]mask) rather than
            // a fixed convention - on little-endian x86 the common case matches
            // DirectShow's BGRA layout, but if colors come out channel-swapped on
            // some source, that mask is the first thing to check.
            CopyPicturePlanes(p_outpic, p_pic);
            PixelOps::ProcessFrameRGB32(
                p_outpic->p[0].p_pixels, width, height, p_outpic->p[0].i_pitch,
                effectiveColorInfo, sys->toneMap, SelectCpuLut(sys, effectiveColorInfo.space));
            picture_Release(p_pic);
            return p_outpic;
        }

        // Shouldn't happen - Open() only accepts the three chromas above - but
        // fail safe to a plain copy rather than an uninitialized/garbage frame.
        CopyPicturePlanes(p_outpic, p_pic);
        picture_Release(p_pic);
        return p_outpic;
    }

    // --- Live variable callbacks (the hotkey replacements) ----------------------
    int EnabledCallback(vlc_object_t*, const char*, vlc_value_t, vlc_value_t newval, void* data)
    {
        auto* sys = reinterpret_cast<filter_sys_t*>(data);
        sys->enabled.store(newval.b_bool != 0, std::memory_order_relaxed);
        return VLC_SUCCESS;
    }

    int ForceSpaceCallback(vlc_object_t*, const char*, vlc_value_t, vlc_value_t newval, void* data)
    {
        auto* sys = reinterpret_cast<filter_sys_t*>(data);
        sys->lutOverride.store(newval.i_int, std::memory_order_relaxed);
        return VLC_SUCCESS;
    }

    int Open(vlc_object_t* p_this)
    {
        auto* p_filter = reinterpret_cast<filter_t*>(p_this);

        const vlc_fourcc_t chroma = p_filter->fmt_in.video.i_chroma;
        if (chroma != VLC_CODEC_NV12 && chroma != VLC_CODEC_P010 && chroma != VLC_CODEC_RGB32)
            return VLC_EGENERIC;
        if (p_filter->fmt_in.video.i_chroma != p_filter->fmt_out.video.i_chroma)
            return VLC_EGENERIC;

        auto* sys = new (std::nothrow) filter_sys_t();
        if (!sys)
            return VLC_ENOMEM;

        char* lut709 = var_InheritString(p_filter, CFG_PREFIX "lut709");
        char* lut2020 = var_InheritString(p_filter, CFG_PREFIX "lut2020");
        char* lutDciP3 = var_InheritString(p_filter, CFG_PREFIX "lutdcip3");
        if (lut709)   { sys->pathLut709 = lut709;   free(lut709); }
        if (lut2020)  { sys->pathLut2020 = lut2020;  free(lut2020); }
        if (lutDciP3) { sys->pathLutDciP3 = lutDciP3; free(lutDciP3); }

        sys->forceP010AsHlg = var_InheritBool(p_filter, CFG_PREFIX "force-p010-hlg");
        sys->outputTransferFunc709 = var_InheritBool(p_filter, CFG_PREFIX "output-709");
        sys->applyHlgOotf = var_InheritBool(p_filter, CFG_PREFIX "apply-hlg-ootf");
        sys->gpuEngine.SetApplyHlgOotf(sys->applyHlgOotf);
        sys->enabled.store(var_InheritBool(p_filter, CFG_PREFIX "enabled"), std::memory_order_relaxed);

        p_filter->p_sys = sys;
        p_filter->pf_video_filter = Filter;

        // Live toggles: var_Create + var_AddCallback makes these show up (and be
        // changeable mid-playback) through VLC's normal variable machinery -
        // command line, telnet/Lua interface, or a small custom qt/skin control.
        // This is the direct functional replacement for the DirectShow build's
        // five global RegisterHotKey combos; see the file header comment.
        var_Create(p_filter, CFG_PREFIX "enabled", VLC_VAR_BOOL);
        var_SetBool(p_filter, CFG_PREFIX "enabled", sys->enabled.load(std::memory_order_relaxed));
        var_AddCallback(p_filter, CFG_PREFIX "enabled", EnabledCallback, sys);

        var_Create(p_filter, CFG_PREFIX "force-space", VLC_VAR_INTEGER);
        var_SetInteger(p_filter, CFG_PREFIX "force-space", -1);
        var_AddCallback(p_filter, CFG_PREFIX "force-space", ForceSpaceCallback, sys);

        return VLC_SUCCESS;
    }

    void Close(vlc_object_t* p_this)
    {
        auto* p_filter = reinterpret_cast<filter_t*>(p_this);
        auto* sys = reinterpret_cast<filter_sys_t*>(p_filter->p_sys);

        var_DelCallback(p_filter, CFG_PREFIX "enabled", EnabledCallback, sys);
        var_DelCallback(p_filter, CFG_PREFIX "force-space", ForceSpaceCallback, sys);

        delete sys;
    }
}

#define ENABLED_TEXT N_("Enable LUT/HDR processing")
#define LUT709_TEXT N_("BT.709 3D LUT file")
#define LUT2020_TEXT N_("BT.2020 3D LUT file")
#define LUTDCIP3_TEXT N_("DCI-P3 3D LUT file")
#define FORCE_P010_TEXT N_("Treat unflagged 4K P010 as HLG")
#define FORCE_P010_LONGTEXT N_("Off by default - only enable if you knowingly have " \
    "unlabeled 4K HLG source and want it force-processed. Mirrors " \
    "HKCU\\Software\\LutHdrFilter\\ForceP010AsHLG from the MPC-BE build.")
#define OUTPUT_709_TEXT N_("Signal output as BT.709 transfer (instead of pure 2.2 gamma)")
#define OOTF_TEXT N_("Apply HLG OOTF before the LUT")

vlc_module_begin()
    set_shortname(N_("LUT/HDR Color Filter"))
    set_description(N_("HDR tone-map + 3D LUT color correction (HLG sources)"))
    set_capability("video filter", 0)
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)

    add_bool(CFG_PREFIX "enabled", true, ENABLED_TEXT, ENABLED_TEXT, false)
    add_loadfile(CFG_PREFIX "lut709", nullptr, LUT709_TEXT, LUT709_TEXT)
    add_loadfile(CFG_PREFIX "lut2020", nullptr, LUT2020_TEXT, LUT2020_TEXT)
    add_loadfile(CFG_PREFIX "lutdcip3", nullptr, LUTDCIP3_TEXT, LUTDCIP3_TEXT)
    add_bool(CFG_PREFIX "force-p010-hlg", false, FORCE_P010_TEXT, FORCE_P010_LONGTEXT, false)
    add_bool(CFG_PREFIX "output-709", false, OUTPUT_709_TEXT, OUTPUT_709_TEXT, false)
    add_bool(CFG_PREFIX "apply-hlg-ootf", false, OOTF_TEXT, OOTF_TEXT, false)

    add_shortcut("lut_hdr")
    set_callbacks(Open, Close)
vlc_module_end()
