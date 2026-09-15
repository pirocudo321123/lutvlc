// YuvOps.cpp
#include "YuvOps.h"
#include <algorithm>
#include <cmath>

namespace
{
    // Converts normalized (already range-expanded) Y/Cb/Cr — Y in [0,1], Cb/Cr
    // centered on 0 in roughly [-0.5,0.5] — to RGB using the given Kr/Kb coefficients.
    // Standard ITU-R BT.601/709/2020-family inverse matrix.
    inline RgbF YCbCrToRgb(float y, float cb, float cr, float kr, float kb)
    {
        const float r = y + 2.0f * (1.0f - kr) * cr;
        const float b = y + 2.0f * (1.0f - kb) * cb;
        const float g = (y - kr * r - kb * b) / (1.0f - kr - kb);
        return { r, g, b };
    }

    inline void RgbToYCbCr(RgbF rgb, float kr, float kb, float& y, float& cb, float& cr)
    {
        y = kr * rgb.r + (1.0f - kr - kb) * rgb.g + kb * rgb.b;
        cb = (rgb.b - y) / (2.0f * (1.0f - kb));
        cr = (rgb.r - y) / (2.0f * (1.0f - kr));
    }

    // --- 8-bit (NV12) range conversion ---------------------------------------
    inline void NormalizeYCbCr8(uint8_t y8, uint8_t cb8, uint8_t cr8, bool limited,
                                 float& y, float& cb, float& cr)
    {
        if (limited)
        {
            y  = (y8  - 16.0f) / 219.0f;
            cb = (cb8 - 128.0f) / 224.0f;
            cr = (cr8 - 128.0f) / 224.0f;
        }
        else
        {
            y  = y8 / 255.0f;
            cb = (cb8 - 128.0f) / 255.0f;
            cr = (cr8 - 128.0f) / 255.0f;
        }
    }

    inline void DenormalizeYCbCr8(float y, float cb, float cr, bool limited,
                                   uint8_t& y8, uint8_t& cb8, uint8_t& cr8)
    {
        auto clampByte = [](float v) {
            return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
        };
        if (limited)
        {
            y8  = clampByte(y * 219.0f + 16.0f);
            cb8 = clampByte(cb * 224.0f + 128.0f);
            cr8 = clampByte(cr * 224.0f + 128.0f);
        }
        else
        {
            y8  = clampByte(y * 255.0f);
            cb8 = clampByte(cb * 255.0f + 128.0f);
            cr8 = clampByte(cr * 255.0f + 128.0f);
        }
    }

    // --- 10-bit (P010, value stored as raw10 << 6 in a 16-bit word) ----------
    inline void NormalizeYCbCr10(uint16_t y10, uint16_t cb10, uint16_t cr10, bool limited,
                                  float& y, float& cb, float& cr)
    {
        if (limited)
        {
            y  = (y10  - 64.0f) / 876.0f;  // black=64, white=940 (10-bit)
            cb = (cb10 - 512.0f) / 896.0f; // chroma 64-960, centered 512
            cr = (cr10 - 512.0f) / 896.0f;
        }
        else
        {
            y  = y10 / 1023.0f;
            cb = (cb10 - 512.0f) / 1023.0f;
            cr = (cr10 - 512.0f) / 1023.0f;
        }
    }

    inline void DenormalizeYCbCr10(float y, float cb, float cr, bool limited,
                                    uint16_t& y10, uint16_t& cb10, uint16_t& cr10)
    {
        auto clamp10 = [](float v) {
            return static_cast<uint16_t>(std::clamp(v, 0.0f, 1023.0f) + 0.5f);
        };
        if (limited)
        {
            y10  = clamp10(y * 876.0f + 64.0f);
            cb10 = clamp10(cb * 896.0f + 512.0f);
            cr10 = clamp10(cr * 896.0f + 512.0f);
        }
        else
        {
            y10  = clamp10(y * 1023.0f);
            cb10 = clamp10(cb * 1023.0f + 512.0f);
            cr10 = clamp10(cr * 1023.0f + 512.0f);
        }
    }
}

YuvMatrixCoeffs GetMatrixCoeffs(SourceMatrix matrix)
{
    switch (matrix)
    {
    case SourceMatrix::BT601:  return { 0.299f,  0.114f };
    case SourceMatrix::BT2020: return { 0.2627f, 0.0593f };
    case SourceMatrix::BT709:
    case SourceMatrix::Unknown:
    default:                   return { 0.2126f, 0.0722f };
    }
}

void ProcessFrameNV12(
    uint8_t* yPlane,
    uint8_t* uvPlane,
    int width,
    int height,
    int yStride,
    int uvStride,
    const DetectedColorInfo& colorInfo,
    const ToneMapSettings& toneMap,
    const Lut3D& lut)
{
    const YuvMatrixCoeffs coeffs = GetMatrixCoeffs(colorInfo.matrix);
    const bool limited = IsLimitedRange(colorInfo);

    // NV12 chroma is subsampled 2x2 (4:2:0): one UV sample serves a 2x2 luma block.
    // We process each block in one pass: read the shared chroma sample once, convert
    // each of the (up to 4) luma samples to RGB using it, tone-map + LUT each result
    // independently (so per-pixel highlight/shadow detail from luma is preserved),
    // convert each back to YCbCr, write the new Y values individually, and write back
    // chroma as the average of the block's new Cb/Cr. Single pass, no extra buffers.
    for (int by = 0; by < height; by += 2)
    {
        for (int bx = 0; bx < width; bx += 2)
        {
            uint8_t* uv = uvPlane + (by / 2) * uvStride + (bx / 2) * 2;
            const uint8_t cb8 = uv[0];
            const uint8_t cr8 = uv[1];

            float cbSum = 0.0f, crSum = 0.0f;
            int count = 0;

            for (int dy = 0; dy < 2 && (by + dy) < height; ++dy)
            {
                for (int dx = 0; dx < 2 && (bx + dx) < width; ++dx)
                {
                    uint8_t* yPixel = yPlane + static_cast<size_t>(by + dy) * yStride + (bx + dx);

                    float yy, ccb, ccr;
                    NormalizeYCbCr8(*yPixel, cb8, cr8, limited, yy, ccb, ccr);

                    RgbF rgb = YCbCrToRgb(yy, ccb, ccr, coeffs.kr, coeffs.kb);
                    rgb.r = std::clamp(rgb.r, 0.0f, 1.0f);
                    rgb.g = std::clamp(rgb.g, 0.0f, 1.0f);
                    rgb.b = std::clamp(rgb.b, 0.0f, 1.0f);

                    RgbF processed = PixelOps::ApplyToneMapAndLut(rgb, colorInfo, toneMap, lut);

                    float ny, ncb, ncr;
                    RgbToYCbCr(processed, coeffs.kr, coeffs.kb, ny, ncb, ncr);

                    uint8_t ny8, ncb8, ncr8;
                    DenormalizeYCbCr8(ny, ncb, ncr, limited, ny8, ncb8, ncr8);

                    *yPixel = ny8;
                    cbSum += ncb8;
                    crSum += ncr8;
                    ++count;
                }
            }

            if (count > 0)
            {
                uv[0] = static_cast<uint8_t>(cbSum / count + 0.5f);
                uv[1] = static_cast<uint8_t>(crSum / count + 0.5f);
            }
        }
    }
}

void ProcessFrameP010(
    uint16_t* yPlane,
    uint16_t* uvPlane,
    int width,
    int height,
    int yStrideBytes,
    int uvStrideBytes,
    const DetectedColorInfo& colorInfo,
    const ToneMapSettings& toneMap,
    const Lut3D& lut)
{
    const YuvMatrixCoeffs coeffs = GetMatrixCoeffs(colorInfo.matrix);
    const bool limited = IsLimitedRange(colorInfo);

    const int yStrideSamples = yStrideBytes / 2;
    const int uvStrideSamples = uvStrideBytes / 2;

    for (int by = 0; by < height; by += 2)
    {
        for (int bx = 0; bx < width; bx += 2)
        {
            uint16_t* uv = uvPlane + static_cast<size_t>(by / 2) * uvStrideSamples + (bx / 2) * 2;
            // P010 stores its 10-bit value in the top 10 bits of each 16-bit word.
            const uint16_t cb10 = uv[0] >> 6;
            const uint16_t cr10 = uv[1] >> 6;

            float cbSum = 0.0f, crSum = 0.0f;
            int count = 0;

            for (int dy = 0; dy < 2 && (by + dy) < height; ++dy)
            {
                for (int dx = 0; dx < 2 && (bx + dx) < width; ++dx)
                {
                    uint16_t* yPixel = yPlane + static_cast<size_t>(by + dy) * yStrideSamples + (bx + dx);
                    const uint16_t y10 = *yPixel >> 6;

                    float yy, ccb, ccr;
                    NormalizeYCbCr10(y10, cb10, cr10, limited, yy, ccb, ccr);

                    RgbF rgb = YCbCrToRgb(yy, ccb, ccr, coeffs.kr, coeffs.kb);
                    rgb.r = std::clamp(rgb.r, 0.0f, 1.0f);
                    rgb.g = std::clamp(rgb.g, 0.0f, 1.0f);
                    rgb.b = std::clamp(rgb.b, 0.0f, 1.0f);

                    RgbF processed = PixelOps::ApplyToneMapAndLut(rgb, colorInfo, toneMap, lut);

                    float ny, ncb, ncr;
                    RgbToYCbCr(processed, coeffs.kr, coeffs.kb, ny, ncb, ncr);

                    uint16_t ny10, ncb10, ncr10;
                    DenormalizeYCbCr10(ny, ncb, ncr, limited, ny10, ncb10, ncr10);

                    *yPixel = static_cast<uint16_t>(ny10 << 6);
                    cbSum += ncb10;
                    crSum += ncr10;
                    ++count;
                }
            }

            if (count > 0)
            {
                const uint16_t avgCb = static_cast<uint16_t>(cbSum / count + 0.5f);
                const uint16_t avgCr = static_cast<uint16_t>(crSum / count + 0.5f);
                uv[0] = static_cast<uint16_t>(avgCb << 6);
                uv[1] = static_cast<uint16_t>(avgCr << 6);
            }
        }
    }
}

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
    const Lut3D& lut)
{
    const YuvMatrixCoeffs coeffs = GetMatrixCoeffs(colorInfo.matrix);
    const bool limited = IsLimitedRange(colorInfo);

    const int yStrideSamples = yStrideBytes / 2;
    const int uStrideSamples = uStrideBytes / 2;
    const int vStrideSamples = vStrideBytes / 2;

    // I420 planar 4:2:0: U/V are each quarter-resolution, one sample per 2x2 luma
    // block - same block structure as ProcessFrameP010's shared UV sample above,
    // just split across two planes instead of interleaved in one.
    for (int by = 0; by < height; by += 2)
    {
        for (int bx = 0; bx < width; bx += 2)
        {
            const int ux = bx / 2;
            const int uy = by / 2;
            uint16_t* uSample = uPlane + static_cast<size_t>(uy) * uStrideSamples + ux;
            uint16_t* vSample = vPlane + static_cast<size_t>(uy) * vStrideSamples + ux;

            // NOTE: no >>6 here, unlike ProcessFrameP010 above - I420_10L samples
            // are already right-aligned in [0,1023].
            const uint16_t cb10 = *uSample;
            const uint16_t cr10 = *vSample;

            float cbSum = 0.0f, crSum = 0.0f;
            int count = 0;

            for (int dy = 0; dy < 2 && (by + dy) < height; ++dy)
            {
                for (int dx = 0; dx < 2 && (bx + dx) < width; ++dx)
                {
                    uint16_t* yPixel = yPlane + static_cast<size_t>(by + dy) * yStrideSamples + (bx + dx);
                    const uint16_t y10 = *yPixel;

                    float yy, ccb, ccr;
                    NormalizeYCbCr10(y10, cb10, cr10, limited, yy, ccb, ccr);

                    RgbF rgb = YCbCrToRgb(yy, ccb, ccr, coeffs.kr, coeffs.kb);
                    rgb.r = std::clamp(rgb.r, 0.0f, 1.0f);
                    rgb.g = std::clamp(rgb.g, 0.0f, 1.0f);
                    rgb.b = std::clamp(rgb.b, 0.0f, 1.0f);

                    RgbF processed = PixelOps::ApplyToneMapAndLut(rgb, colorInfo, toneMap, lut);

                    float ny, ncb, ncr;
                    RgbToYCbCr(processed, coeffs.kr, coeffs.kb, ny, ncb, ncr);

                    uint16_t ny10, ncb10, ncr10;
                    DenormalizeYCbCr10(ny, ncb, ncr, limited, ny10, ncb10, ncr10);

                    *yPixel = ny10; // right-aligned, no <<6 - see note above
                    cbSum += ncb10;
                    crSum += ncr10;
                    ++count;
                }
            }

            if (count > 0)
            {
                *uSample = static_cast<uint16_t>(cbSum / count + 0.5f);
                *vSample = static_cast<uint16_t>(crSum / count + 0.5f);
            }
        }
    }
}
