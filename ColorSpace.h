// ColorSpace.h
// Decodes the color primaries / transfer function / matrix / range carried in an
// AM_MEDIA_TYPE's VIDEOINFOHEADER2.dwControlFlags.

#pragma once

#include <streams.h>
#include <dvdmedia.h>   // VIDEOINFOHEADER2, AMCONTROL_COLORINFO_PRESENT

struct PackedExtendedFormat
{
    unsigned int SampleFormat : 8;
    unsigned int VideoChromaSubsampling : 4;
    unsigned int NominalRange : 3;
    unsigned int VideoTransferMatrix : 3;
    unsigned int VideoLighting : 4;
    unsigned int VideoPrimaries : 5;
    unsigned int VideoTransferFunction : 5;
};

namespace MFPrimaries
{
    constexpr unsigned int BT709  = 2;
    constexpr unsigned int BT2020 = 9;
    constexpr unsigned int DCI_P3 = 11;
}

namespace MFTransferFunc
{
    constexpr unsigned int Func22   = 4;
    constexpr unsigned int Func709  = 5;
    constexpr unsigned int Func2084 = 15; // ST.2084 / PQ (MediaFoundation standard value)
    constexpr unsigned int FuncHLG  = 16; // ARIB STD-B67 / HLG (MediaFoundation standard value)
}

namespace MFMatrix
{
    constexpr unsigned int BT709     = 1;
    constexpr unsigned int BT601     = 2;
    constexpr unsigned int BT2020_10 = 4;
    constexpr unsigned int BT2020_12 = 5;
}

namespace MFRange
{
    constexpr unsigned int Full    = 1; // 0-255
    constexpr unsigned int Limited = 2; // 16-235 / 16-240
}

enum class SourceColorSpace
{
    Unknown,
    BT709,
    BT2020,
    DCI_P3,
};

enum class SourceTransfer
{
    Unknown,
    SDR_Gamma,
    PQ,
    HLG,
};

enum class SourceMatrix
{
    Unknown,
    BT601,
    BT709,
    BT2020,
};

enum class SourceRange
{
    Unknown,
    Limited,
    Full,
};

struct DetectedColorInfo
{
    SourceColorSpace space = SourceColorSpace::Unknown;
    SourceTransfer   transfer = SourceTransfer::Unknown;
    SourceMatrix     matrix = SourceMatrix::Unknown;
    SourceRange      range = SourceRange::Unknown;
    bool             present = false;
};

inline bool IsLimitedRange(const DetectedColorInfo& info)
{
    return info.range != SourceRange::Full;
}

inline DetectedColorInfo DecodeColorInfo(const VIDEOINFOHEADER2* vih2)
{
    DetectedColorInfo info;

    if (!vih2)
        return info;

    if (!(vih2->dwControlFlags & AMCONTROL_COLORINFO_PRESENT))
        return info;

    const PackedExtendedFormat* ext =
        reinterpret_cast<const PackedExtendedFormat*>(&vih2->dwControlFlags);

    info.present = true;

    switch (ext->VideoPrimaries)
    {
    case MFPrimaries::BT709:  info.space = SourceColorSpace::BT709;  break;
    case MFPrimaries::BT2020: info.space = SourceColorSpace::BT2020; break;
    case MFPrimaries::DCI_P3: info.space = SourceColorSpace::DCI_P3; break;
    default:                  info.space = SourceColorSpace::Unknown; break;
    }

    switch (ext->VideoTransferFunction)
    {
    case MFTransferFunc::Func2084: info.transfer = SourceTransfer::PQ;  break;
    case MFTransferFunc::FuncHLG:  info.transfer = SourceTransfer::HLG; break;
    case MFTransferFunc::Func22:
    case MFTransferFunc::Func709:  info.transfer = SourceTransfer::SDR_Gamma; break;
    default:                       info.transfer = SourceTransfer::Unknown;  break;
    }

    switch (ext->VideoTransferMatrix)
    {
    case MFMatrix::BT601:     info.matrix = SourceMatrix::BT601; break;
    case MFMatrix::BT709:     info.matrix = SourceMatrix::BT709; break;
    case MFMatrix::BT2020_10:
    case MFMatrix::BT2020_12: info.matrix = SourceMatrix::BT2020; break;
    default:                  info.matrix = SourceMatrix::Unknown; break;
    }

    switch (ext->NominalRange)
    {
    case MFRange::Full:    info.range = SourceRange::Full;    break;
    case MFRange::Limited: info.range = SourceRange::Limited; break;
    default:               info.range = SourceRange::Unknown; break;
    }

    return info;
}