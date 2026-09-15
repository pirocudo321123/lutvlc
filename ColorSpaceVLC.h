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
// NOTE ON ENUM NAMES: video_color_primaries_t / video_transfer_func_t /
// video_color_space_t / video_color_range_t member names have shifted slightly
// across VLC releases (e.g. TRANSFER_FUNC_ARIB_B67 vs TRANSFER_FUNC_HLG for the
// same HLG curve). The names below match the VLC 3.0.x series this plugin targets
// (see build.yml's VLC_VERSION). If your SDK's vlc_es.h disagrees on a name, that
// mismatch will fail loudly at compile time right here - it's a one-line fix,
// not a sign anything else is wrong.

#pragma once

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
#if defined(COLOR_PRIMARIES_DCI_P3)
    case COLOR_PRIMARIES_DCI_P3: info.space = SourceColorSpace::DCI_P3; break;
#endif
    default:                     info.space = SourceColorSpace::Unknown; break;
    }

    switch (fmt->transfer)
    {
    case TRANSFER_FUNC_SMPTE_ST2084: info.transfer = SourceTransfer::PQ;  break;
#if defined(TRANSFER_FUNC_HLG)
    case TRANSFER_FUNC_HLG:          info.transfer = SourceTransfer::HLG; break;
#elif defined(TRANSFER_FUNC_ARIB_B67)
    case TRANSFER_FUNC_ARIB_B67:     info.transfer = SourceTransfer::HLG; break;
#endif
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

    switch (fmt->color_range)
    {
    case COLOR_RANGE_FULL:    info.range = SourceRange::Full;    break;
    case COLOR_RANGE_LIMITED: info.range = SourceRange::Limited; break;
    default:                  info.range = SourceRange::Unknown; break;
    }

    return info;
}
