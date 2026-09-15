// PixelOps.h
// Per-pixel HDR decode / tone-map / LUT-apply pipeline, plus the RGB32 frame loop
// that drives it. YuvOps.cpp reuses ApplyToneMapAndLut for NV12/P010 frames so both
// paths share one color pipeline instead of duplicating it.

#pragma once

#include <cstdint>
#include "Lut3D.h"
#include "ColorSpace.h"

struct ToneMapSettings
{
    bool  enabled = true;
    float maxNits = 1000.0f;   // assumed peak luminance of the HDR source
    float targetNits = 100.0f; // SDR reference white we're mapping down to
};

namespace PixelOps
{
    // ST.2084 (PQ) inverse EOTF: PQ code value [0,1] -> linear light, normalized so
    // that 1.0 = 10000 nits (the PQ reference peak).
    float PQ_ToLinear(float pqValue);

    // ARIB STD-B67 / BT.2100 HLG inverse OETF: HLG code value [0,1] -> scene-linear
    // light, normalized so nominal reference white ~= 1.0 (values above 1 represent
    // HLG's extended highlight headroom, up to ~12x).
    float HLG_ToSceneLinear(float hlgValue);

    // Approximates BT.2100's HLG system-gamma OOTF (opto-optical transfer function),
    // which is a function of the assumed nominal display peak luminance:
    //   gamma = 1.2 + 0.42 * log10(peakNits / 1000)
    // NOTE: this is applied per-channel here rather than via the fully spec-accurate
    // luminance-weighted OOTF (which computes scene luminance Ys = 0.2627R+0.6780G+
    // 0.0593B and scales all three channels by one factor derived from Ys). Per-channel
    // application is a documented simplification — close for near-neutral content, but
    // can introduce a small saturation/hue shift on strongly saturated highlights.
    float HLG_SystemGamma(float nominalPeakNits);

    // Reinhard-extended tone curve: maps unbounded linear light down to [0,1] display
    // range, knee shaped by Lwhite (the luminance that maps to display white).
    float ToneMapReinhardExtended(float linear, float lWhite);

    float LinearToGamma(float linear);
    float GammaToLinear(float gammaValue);

    // Runs one pixel (already split into normalized [0,1] RGB) through HDR decode
    // (if applicable) + tone mapping (if enabled) + the given 3D LUT. Shared by the
    // RGB32 frame loop below and by YuvOps' NV12/P010 processing.
    RgbF ApplyToneMapAndLut(RgbF px, const DetectedColorInfo& info,
                             const ToneMapSettings& toneMap, const Lut3D& lut);

    // Processes one RGB32/ARGB32 frame in place: srcDst is BGRA8 (DirectShow's default
    // RGB32 byte order), stride in bytes, width/height in pixels.
    void ProcessFrameRGB32(
        uint8_t* srcDst,
        int width,
        int height,
        int strideBytes,
        const DetectedColorInfo& colorInfo,
        const ToneMapSettings& toneMap,
        const Lut3D& lut);
}
