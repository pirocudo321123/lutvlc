// ColorSpace.h
// Platform-neutral color-info types shared by both the DirectShow filter and the
// VLC plugin. Deliberately has ZERO DirectShow dependency (no <streams.h>,
// <dvdmedia.h>, VIDEOINFOHEADER2) so it compiles standalone in either build.
// The DirectShow-specific decode function that used to live here has moved to
// ColorSpaceDirectShow.h - include that ADDITIONALLY in DirectShow-only files
// (currently just LutHdrFilter.cpp) if you need DecodeColorInfo(VIDEOINFOHEADER2*).

#pragma once

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