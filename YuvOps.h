// YuvOps.h
// NV12 (8-bit, 4:2:0, semi-planar) and P010 (10-bit-in-16-bit storage, 4:2:0,
// semi-planar) frame processing: YUV<->RGB conversion (matrix- and range-aware),
// tone-mapping + LUT application (reusing PixelOps::ApplyToneMapAndLut), and
// chroma-subsampled write-back, done as a single in-place pass over 2x2 luma blocks.

#pragma once

#include <cstdint>
#include "Lut3D.h"
#include "ColorSpace.h"
#include "PixelOps.h"

struct YuvMatrixCoeffs
{
    float kr;
    float kb;
};

// Kr/Kb luma coefficients for the given matrix. Falls back to BT.709 for Unknown,
// since it's the most common matrix for non-4K/non-HDR content and BT.2020 sources
// overwhelmingly signal their matrix explicitly (so an Unknown BT.2020 clip is rare).
YuvMatrixCoeffs GetMatrixCoeffs(SourceMatrix matrix);

// Processes an NV12 frame in place. yPlane/uvPlane point at the start of each plane
// within the same buffer (uvPlane == yPlane + yStride*height for a standard
// contiguous NV12 layout, but callers pass them explicitly for clarity).
//
// ASSUMPTION: this implementation assumes yStride == width and uvStride == width
// (no extra row padding beyond the frame's own dimensions). Some decoders introduce
// alignment padding (e.g. rounding stride up to 16 or 32 bytes) that this does not
// currently detect or handle — a real gap if you hit decoders that pad, flagged here
// rather than silently producing corrupted-looking output.
void ProcessFrameNV12(
    uint8_t* yPlane,
    uint8_t* uvPlane,
    int width,
    int height,
    int yStride,
    int uvStride,
    const DetectedColorInfo& colorInfo,
    const ToneMapSettings& toneMap,
    const Lut3D& lut);

// Processes a P010 frame in place. Same layout/stride assumptions as NV12 above, but
// each sample is a 16-bit word holding a 10-bit value left-shifted by 6 bits (the
// P010 convention) rather than an 8-bit byte. Strides here are in bytes, not samples.
void ProcessFrameP010(
    uint16_t* yPlane,
    uint16_t* uvPlane,
    int width,
    int height,
    int yStrideBytes,
    int uvStrideBytes,
    const DetectedColorInfo& colorInfo,
    const ToneMapSettings& toneMap,
    const Lut3D& lut);

// Processes a planar 10-bit 4:2:0 frame (VLC_CODEC_I420_10L - separate Y/U/V
// planes, each with its own stride) in place. U/V are quarter-resolution, one
// sample per 2x2 luma block - same block structure as NV12/P010's shared UV
// sample, just split across two planes instead of interleaved in one.
//
// IMPORTANT: unlike P010, each 16-bit sample here is RIGHT-aligned (raw value
// in [0,1023], no <<6 MSB shift). This is ffmpeg's yuv420p10le layout, and it's
// what VLC's D3D11 opaque-surface readback (the "d3d11_filters" module) and
// software HEVC 10-bit decode both actually hand out - P010 (semi-planar,
// MSB-aligned) essentially never appears in the VLC video-filter chain in
// practice, which is why this exists alongside ProcessFrameP010 rather than
// reusing it directly.
void ProcessFrameI420_10(
    uint16_t* yPlane,
    uint16_t* uPlane,
    uint16_t* vPlane,
    int width,
    int height,
    int yStrideBytes,
    int uStrideBytes,
    int vStrideBytes,
    const DetectedColorInfo& colorInfo,
    const ToneMapSettings& toneMap,
    const Lut3D& lut);
