// ColorSpaceVLC.h
// Decodes the color primaries / transfer function / matrix / range carried in a
// libVLC video_format_t into the platform-neutral DetectedColorInfo type from
// ColorSpace.h.
//
// Split out from ColorSpace.h for the same reason ColorSpaceDirectShow.h is split
// out on the DirectShow side: ColorSpace.h (and everything downstream of it -
// Lut3D, PixelOps, YuvOps, D3D11LutEngine) must stay buildable with ZERO knowledge
// of either host's SDK. Include THIS header (in addition to ColorSpace.h) only
// from the VLC plugin's own files (currently just lut_hdr_vlc.cpp).
//
// NOTE ON ENUM NAMES: the names below are the VLC 3.0.x ones (see build.yml's
// VLC_VERSION), taken from that series' vlc_es.h. Two of them used to be wrapped
// in `#if defined(...)` guards here, which was a silent no-op trap: they name
// *enumerators*, not macros, so the preprocessor can't see them and `defined()`
// is always false. COLOR_PRIMARIES_DCI_P3 was being compiled out entirely, which
// meant every DCI-P3 source fell through to SourceColorSpace::Unknown and never
// picked up the DCI-P3 LUT. Both are unconditional now. (TRANSFER_FUNC_ARIB_B67
// really is a macro in 3.0.x - an alias for TRANSFER_FUNC_HLG - so the HLG guard
// happened to land on a working branch, but for the wrong reason.)
//
// If a future SDK disagrees on a name, that fails loudly at compile time right
// here - a one-line fix, not a sign anything else is wrong.

#pragma once

#include "VlcMsvcCompat.h" // MUST precede vlc_common.h - see that file's header
#include <vlc_common.h>
#include "ColorSpace.h"

inline DetectedColorInfo DecodeColorInfo(const video_format_t* fmt)
{
    DetectedColorInfo info;

    if (!fmt)
        return info;

    // Unlike DirectShow's single AMCONTROL_COLORINFO_PRESENT flag, VLC has no one
    // bit meaning "color info was signaled at all" - each field defaults to its
    // own *_UNDEF value independently when the demuxer/decoder didn't set it.
    // We treat "present" as "at least primaries or transfer was actually signaled",
    // which is the same practical signal DecodeColorInfo(VIDEOINFOHEADER2*) gives:
    // enough to tell real HDR metadata apart from an unflagged/ordinary stream.
    const bool anySignaled = (fmt->primaries != COLOR_PRIMARIES_UNDEF) ||
                              (fmt->transfer  != TRANSFER_FUNC_UNDEF);
    info.present = anySignaled;

    switch (fmt->primaries)
    {
    case COLOR_PRIMARIES_BT709:  info.space = SourceColorSpace::BT709;  break;
    case COLOR_PRIMARIES_BT2020: info.space = SourceColorSpace::BT2020; break;
    case COLOR_PRIMARIES_DCI_P3: info.space = SourceColorSpace::DCI_P3; break;
    default:                     info.space = SourceColorSpace::Unknown; break;
    }

    switch (fmt->transfer)
    {
    case TRANSFER_FUNC_SMPTE_ST2084: info.transfer = SourceTransfer::PQ;  break;
    case TRANSFER_FUNC_HLG:          info.transfer = SourceTransfer::HLG; break;
    // TRANSFER_FUNC_SRGB is 3.0.x's name for plain gamma 2.2, per vlc_es.h's own
    // comment on it - not a separate piecewise sRGB curve needing its own branch.
    case TRANSFER_FUNC_SRGB:
    case TRANSFER_FUNC_BT709:        info.transfer = SourceTransfer::SDR_Gamma; break;
    default:                         info.transfer = SourceTransfer::Unknown;  break;
    }

    // fmt->space is VLC's name for the YUV matrix coefficients (kr/kb) - NOT the
    // same thing as fmt->primaries, despite the similar-sounding name. Don't
    // conflate the two: a BT.2020-primaries HLG master almost always also signals
    // a BT.2020 matrix, but they're independent fields and this plugin (like the
    // DirectShow build) keys its LUT selection off primaries, only using this for
    // the YCbCr<->RGB conversion itself (GetMatrixCoeffs in YuvOps.cpp).
    switch (fmt->space)
    {
    case COLOR_SPACE_BT601: info.matrix = SourceMatrix::BT601; break;
    case COLOR_SPACE_BT709: info.matrix = SourceMatrix::BT709; break;
    case COLOR_SPACE_BT2020: info.matrix = SourceMatrix::BT2020; break;
    default:                info.matrix = SourceMatrix::Unknown; break;
    }

    // VLC 3.0.x stores range as a plain bool. The video_color_range_t enum with
    // COLOR_RANGE_UNDEF/FULL/LIMITED is a VLC 4.x addition, so there is no
    // "was range actually signaled?" distinction available on 3.0 - an unflagged
    // stream and an explicitly-limited one both arrive here as false.
    //
    // Collapsing false to Limited (rather than Unknown) is therefore the honest
    // mapping AND a behavioral no-op: IsLimitedRange() in ColorSpace.h already
    // treats anything that isn't Full as limited, which is the right default for
    // video - full-range YUV is the rare, explicitly-signaled case.
    info.range = fmt->b_color_range_full ? SourceRange::Full : SourceRange::Limited;

    return info;
}
