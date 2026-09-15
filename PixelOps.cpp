// PixelOps.cpp
#include "PixelOps.h"
#include <cmath>
#include <algorithm>
#include <ppl.h>

namespace
{
    // ITU-R BT.2407 standard matrix: Linear BT.2020 -> Linear BT.709 gamut
    inline RgbF Convert_BT2020_to_BT709(RgbF c)
    {
        float r =  1.6605f * c.r - 0.5876f * c.g - 0.0728f * c.b;
        float g = -0.1246f * c.r + 1.1329f * c.g - 0.0083f * c.b;
        float b = -0.0182f * c.r - 0.1006f * c.g + 1.1187f * c.b;
        return { std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f) };
    }
}

namespace PixelOps
{
    // (Unused math functions removed for clarity and performance)
    float PQ_ToLinear(float pqValue) { return 0.0f; }
    float HLG_ToSceneLinear(float hlgValue) { return 0.0f; }
    float HLG_SystemGamma(float nominalPeakNits) { return 0.0f; }
    float ToneMapReinhardExtended(float linear, float lWhite) { return 0.0f; }
    float LinearToGamma(float linear) { return 0.0f; }
    float GammaToLinear(float gammaValue) { return 0.0f; }

    RgbF ApplyToneMapAndLut(RgbF px, const DetectedColorInfo& info,
                             const ToneMapSettings& toneMap, const Lut3D& lut)
    {
        // 1. If a valid .cube text file is loaded, apply it! (Zero math lag)
        if (lut.IsLoaded())
        {
            return lut.Apply(px);
        }

        // 2. If the LUT failed to load (e.g., it was a renamed .3dlut file),
        // DO NOTHING except color gamut conversion. 
        // This guarantees 0% CPU lag so you know the math isn't running.
        if (info.space == SourceColorSpace::BT2020)
        {
            px = Convert_BT2020_to_BT709(px);
        }

        return px;
    }

    void ProcessFrameRGB32(
        uint8_t* srcDst,
        int width,
        int height,
        int strideBytes,
        const DetectedColorInfo& colorInfo,
        const ToneMapSettings& toneMap,
        const Lut3D& lut)
    {
        concurrency::parallel_for(0, height, [&](int y)
        {
            uint8_t* row = srcDst + static_cast<size_t>(y) * strideBytes;
            for (int x = 0; x < width; ++x)
            {
                uint8_t* px = row + static_cast<size_t>(x) * 4;
                RgbF color{
                    px[2] / 255.0f,
                    px[1] / 255.0f,
                    px[0] / 255.0f
                };

                color = ApplyToneMapAndLut(color, colorInfo, toneMap, lut);

                px[2] = static_cast<uint8_t>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[1] = static_cast<uint8_t>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f + 0.5f);
                px[0] = static_cast<uint8_t>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        });
    }
}