#include <initguid.h>
#include "LutHdrFilter.h"
#include "LutHdrPropPage.h"
#include "FourCCSubtype.h"
#include <algorithm>
#include <cstdio>
#include <cstdarg>

namespace
{
    // Debug-only telemetry. View with DebugView (Sysinternals), filter on "LUTHDR:".
    // Compiled into Release too for now, deliberately, so we can actually see what's
    // happening on the reporter's machine without needing a separate debug build.
    void LutHdrTrace(const wchar_t* fmt, ...)
    {
        wchar_t buf[512];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        OutputDebugStringW(L"LUTHDR: ");
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");
    }

    const wchar_t* ToStr(SourceColorSpace s)
    {
        switch (s) { case SourceColorSpace::BT709: return L"BT709"; case SourceColorSpace::BT2020: return L"BT2020";
                      case SourceColorSpace::DCI_P3: return L"DCI_P3"; default: return L"Unknown"; }
    }
    const wchar_t* ToStr(SourceTransfer t)
    {
        switch (t) { case SourceTransfer::SDR_Gamma: return L"SDR_Gamma"; case SourceTransfer::PQ: return L"PQ";
                      case SourceTransfer::HLG: return L"HLG"; default: return L"Unknown"; }
    }
    const wchar_t* ToStr(SourceMatrix m)
    {
        switch (m) { case SourceMatrix::BT601: return L"BT601"; case SourceMatrix::BT709: return L"BT709";
                      case SourceMatrix::BT2020: return L"BT2020"; default: return L"Unknown"; }
    }
    const wchar_t* ToStr(SourceRange r)
    {
        switch (r) { case SourceRange::Limited: return L"Limited"; case SourceRange::Full: return L"Full"; default: return L"Unknown"; }
    }
    const wchar_t* ToStr(PixelFormat f)
    {
        switch (f) { case PixelFormat::RGB32: return L"RGB32"; case PixelFormat::NV12: return L"NV12";
                      case PixelFormat::P010: return L"P010"; default: return L"Unknown"; }
    }

    int AlignUp(int v, int a) { return ((v + a - 1) / a) * a; }

    // Single source of truth for "will this stream ever actually be processed",
    // shared by CheckInputType (decides whether to accept the connection at all)
    // and ShouldProcessCurrentVideo (decides whether to run the GPU path once
    // connected). Given the same inputs both must agree, or CheckInputType could
    // accept a stream that then never gets processed anyway.
    bool ComputeShouldProcess(DetectedColorInfo info, PixelFormat format, int width,
                               bool forceP010AsHlg,
                               const std::wstring& pathLut709,
                               const std::wstring& pathLut2020,
                               const std::wstring& pathLutDCIP3)
    {
        // Same opt-in override as SetMediaType used to apply after the fact: unflagged
        // 4K P010 can be forced to HLG/BT2020 via the registry. Applying it here too
        // means CheckInputType's early accept/reject decision matches exactly what
        // SetMediaType would later compute, instead of duplicating/drifting from it.
        if (!info.present && format == PixelFormat::P010 && width >= 3840 && forceP010AsHlg)
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

    // A media sample's *declared* stride (from the connected media type / our own
    // SetMediaType calc) is only reliable for the pin that negotiated it. The OTHER
    // side's allocator (very commonly a renderer's D3D-surface-backed allocator, as
    // seen with MPC Video Renderer padding NV12 rows to a hardware-aligned pitch) is
    // free to hand back a larger buffer than requested, laid out with row padding we
    // never asked for.
    //
    // For NV12/P010 there's a SECOND, independent padding risk beyond row stride:
    // the decode surface's *coded height* (where the UV plane actually starts) can
    // be taller than the stream's signaled display height, padded up to whatever
    // macroblock/tiling alignment the decoder needed (commonly 16, sometimes more).
    // A source like 750x1000 has height=1000, which is NOT a multiple of 16
    // (1000/16 = 62.5) — a very plausible real coded height for that is 1008.
    // Assuming the UV plane starts at strideBytes*height (the old behavior) instead
    // of strideBytes*codedHeight silently reads 8 rows of tail-end Y data as if it
    // were chroma, producing exactly the kind of speckled/smeared corruption seen
    // on non-16-aligned-height sources — even for a plain SDR passthrough frame
    // that the LUT/GPU pipeline never touches.
    //
    // Both stride and coded height are recovered jointly from the sample's real
    // buffer size, trying the alignments real hardware actually uses, rather than
    // assuming either one individually.
    struct PlaneLayout
    {
        int stride = 0;
        int codedHeight = 0; // where the UV plane actually starts, in rows
    };

    PlaneLayout DeriveNV12P010Layout(long sampleSize, int width, int height, PixelFormat format)
    {
        PlaneLayout result{ (format == PixelFormat::NV12) ? width : width * 2, height }; // tight fallback

        if (width <= 0 || height <= 0 || sampleSize <= 0)
            return result;

        const int tightStride = (format == PixelFormat::NV12) ? width : width * 2;
        static const int kStrideAligns[] = { 1, 16, 32, 64, 128, 256 };
        static const int kHeightAligns[] = { 1, 16, 32, 64 };

        for (int hAlign : kHeightAligns)
        {
            const int candidateHeight = AlignUp(height, hAlign);
            const int candidateUvHeight = (candidateHeight + 1) / 2;
            const long candidateTotalRows = candidateHeight + candidateUvHeight;
            if (candidateTotalRows <= 0 || sampleSize % candidateTotalRows != 0)
                continue;

            const int candidateStride = static_cast<int>(sampleSize / candidateTotalRows);
            if (candidateStride < tightStride)
                continue;

            for (int sAlign : kStrideAligns)
            {
                if (AlignUp(tightStride, sAlign) == candidateStride)
                {
                    result.stride = candidateStride;
                    result.codedHeight = candidateHeight;
                    return result;
                }
            }
        }

        return result; // nothing matched a known alignment pattern -> tight/unpadded
    }

    int DeriveRgb32Stride(long sampleSize, int width, int height)
    {
        const int tight = ((width * 32 + 31) / 32) * 4;
        if (width <= 0 || height <= 0 || sampleSize <= 0)
            return tight;
        if (sampleSize % height == 0)
        {
            const int stride = static_cast<int>(sampleSize / height);
            if (stride >= tight) return stride;
        }
        return tight;
    }

    // Copies one frame from src to dst, honoring each side's OWN stride and coded
    // height independently (they can differ from each other and from the stream's
    // declared width/height — see DeriveNV12P010Layout above). Always plane-aware:
    // no "strides match => flat memcpy" shortcut, because two buffers can share the
    // same row stride while still having different coded heights (padded rows only,
    // no horizontal padding), which a flat memcpy would silently get wrong.
    void CopyFrameRespectingStride(const BYTE* src, BYTE* dst,
                                    int srcStride, int dstStride,
                                    int srcCodedHeight, int dstCodedHeight,
                                    int width, int height, PixelFormat format)
    {
        if (!src || !dst || srcStride <= 0 || dstStride <= 0 || width <= 0 || height <= 0)
            return;

        if (format == PixelFormat::RGB32)
        {
            const int rowBytes = width * 4;
            for (int y = 0; y < height; ++y)
                memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride, rowBytes);
            return;
        }

        // NV12 / P010: two planes, Y full-height, UV subsampled 2x2.
        const int uvHeight = (height + 1) / 2;
        const int rowBytesY = (format == PixelFormat::NV12) ? width : width * 2;
        const int rowBytesUV = rowBytesY; // same byte width: half the horizontal samples, 2 components each

        const BYTE* srcY = src;
        BYTE* dstY = dst;
        for (int y = 0; y < height; ++y)
            memcpy(dstY + static_cast<size_t>(y) * dstStride, srcY + static_cast<size_t>(y) * srcStride, rowBytesY);

        const BYTE* srcUV = src + static_cast<size_t>(srcStride) * srcCodedHeight;
        BYTE* dstUV = dst + static_cast<size_t>(dstStride) * dstCodedHeight;
        for (int y = 0; y < uvHeight; ++y)
            memcpy(dstUV + static_cast<size_t>(y) * dstStride, srcUV + static_cast<size_t>(y) * srcStride, rowBytesUV);
    }
}

DEFINE_GUID(OUR_MEDIASUBTYPE_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(OUR_MEDIASUBTYPE_P010, 0x30313050, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

const AMOVIESETUP_MEDIATYPE sudPinTypes[] =
{
    { &MEDIATYPE_Video, &MEDIASUBTYPE_RGB32 },
    { &MEDIATYPE_Video, &OUR_MEDIASUBTYPE_NV12 },
    { &MEDIATYPE_Video, &OUR_MEDIASUBTYPE_P010 }
};

const AMOVIESETUP_PIN sudPins[] =
{
    { const_cast<LPWSTR>(L"Input"),  FALSE, FALSE, FALSE, FALSE, nullptr, nullptr, 3, sudPinTypes },
    { const_cast<LPWSTR>(L"Output"), FALSE, TRUE,  FALSE, FALSE, nullptr, nullptr, 3, sudPinTypes },
};

const AMOVIESETUP_FILTER sudLutHdrFilter =
{
    &CLSID_LutHdrFilter,
    L"LUT/HDR Color Filter",
    MERIT_DO_NOT_USE, // Previously 0xFF800000 (above MERIT_PREFERRED), which meant ANY
                       // DirectShow app's automatic graph-building (not just MPC-BE)
                       // would silently grab this filter for any matching RGB32/NV12/
                       // P010 content system-wide as soon as it was registered.
                       // MERIT_DO_NOT_USE means normal Intelligent Connect will never
                       // auto-select it. MPC-BE's External Filters "Prefer"+"Insert"
                       // config force-includes it by CLSID regardless of merit, so it
                       // still works exactly the same there.
    2,
    sudPins
};

CFactoryTemplate g_Templates[] =
{
    {
        L"LUT/HDR Color Filter",
        &CLSID_LutHdrFilter,
        CLutHdrFilter::CreateInstance,
        nullptr,
        &sudLutHdrFilter
    },
    {
        L"LUT/HDR Color Filter Property Page",
        &CLSID_LutHdrPropPage,
        CLutHdrPropPage::CreateInstance,
        nullptr,
        nullptr
    }
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

STDAPI DllRegisterServer() { return AMovieDllRegisterServer2(TRUE); }
STDAPI DllUnregisterServer() { return AMovieDllRegisterServer2(FALSE); }

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved)
{
    return DllEntryPoint(static_cast<HINSTANCE>(hModule), dwReason, lpReserved);
}

CUnknown* WINAPI CLutHdrFilter::CreateInstance(LPUNKNOWN punk, HRESULT* phr)
{
    auto* filter = new CLutHdrFilter(punk, phr);
    if (!filter)
        *phr = E_OUTOFMEMORY;
    return filter;
}

STDMETHODIMP CLutHdrFilter::NonDelegatingQueryInterface(REFIID riid, void** ppv)
{
    if (riid == IID_ISpecifyPropertyPages)
        return GetInterface(static_cast<ISpecifyPropertyPages*>(this), ppv);
    return CTransformFilter::NonDelegatingQueryInterface(riid, ppv);
}

STDMETHODIMP CLutHdrFilter::GetPages(CAUUID* pPages)
{
    if (!pPages)
        return E_POINTER;

    pPages->cElems = 1;
    pPages->pElems = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
    if (!pPages->pElems)
        return E_OUTOFMEMORY;

    pPages->pElems[0] = CLSID_LutHdrPropPage;
    return S_OK;
}

CLutHdrFilter::CLutHdrFilter(LPUNKNOWN punk, HRESULT* phr)
    : CTransformFilter(L"LUT/HDR Color Filter", punk, CLSID_LutHdrFilter)
{
    // NOTE: GPU device creation + compute shader compile used to happen right here,
    // unconditionally, for every filter instance. DirectShow's Intelligent Connect
    // routinely constructs and discards several instances of a filter while probing
    // graph combinations before one actually survives - our own trace log shows
    // three constructions within ~1.2s for a single playback start. Each one paid
    // the full D3D11CreateDevice + D3DCompile cost even though only one instance
    // ends up in the final graph, and even THAT surviving instance only needs the
    // GPU engine if the connected content is HLG (see ShouldProcessCurrentVideo) -
    // plain SDR/BT.709 clips like the one that flagged this never call into it at
    // all. Initialize() is now deferred to EnsureLutLoaded(), the one call site that
    // only runs once we already know this instance will actually process a frame.
    LoadConfigFromRegistry();
    _gpuEngine.SetApplyHlgOotf(_applyHlgOotf);
    LutHdrTrace(L"Init: Lut709='%s' Lut2020='%s' LutDCIP3='%s' ForceP010AsHLG=%d OutputTransferFunc=%s ApplyHlgOOTF=%d (GPU device creation deferred until first HLG frame)",
           _pathLut709.c_str(), _pathLut2020.c_str(), _pathLutDCIP3.c_str(), _forceP010AsHlg ? 1 : 0,
           _outputTransferFunc709 ? L"Func709" : L"Func22", _applyHlgOotf ? 1 : 0);

    // Live hotkeys: registered up front (not deferred like the GPU device) so
    // they're active for the whole lifetime of this instance, including before
    // the first HLG frame arrives. Unbound entries (vk==0, the default until the
    // user sets one on the property page) are simply skipped by HotkeyManager,
    // so this is a no-op until the user actually configures a combo.
    LoadHotkeysFromRegistry();
    _hotkeys.Start(
        { _hkToggleEnable, _hkForceAuto, _hkForce709, _hkForce2020, _hkForceDCIP3 },
        [this](int id) { OnHotkeyFired(id); });
}

void CLutHdrFilter::LoadConfigFromRegistry()
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LutHdrFilter", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    auto readPath = [&](const wchar_t* name, std::wstring& outPath)
    {
        wchar_t buf[MAX_PATH]{};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS
            && type == REG_SZ)
        {
            outPath = buf;
        }
    };

    readPath(L"Lut709", _pathLut709);
    readPath(L"Lut2020", _pathLut2020);
    readPath(L"LutDCIP3", _pathLutDCIP3);

    DWORD forceFlag = 0;
    DWORD forceSize = sizeof(forceFlag);
    DWORD forceType = 0;
    if (RegQueryValueExW(key, L"ForceP010AsHLG", nullptr, &forceType,
                          reinterpret_cast<BYTE*>(&forceFlag), &forceSize) == ERROR_SUCCESS
        && forceType == REG_DWORD)
    {
        _forceP010AsHlg = (forceFlag != 0);
    }

    // A/B toggle for the outgoing signaled transfer function, since it's unclear
    // whether these particular .3dlut files were authored assuming a pure power-law
    // 2.2 gamma target or the standard BT.709 OETF (linear toe + ~0.45 power) — a
    // mismatch there can show up as a subtle overall color cast even when the pixel
    // math itself is otherwise correct.
    //   HKCU\Software\LutHdrFilter\OutputTransferFunc (REG_DWORD): 0 = Func22 (default,
    //   current behavior), 1 = Func709 (BT.709 OETF)
    DWORD outputTransferFlag = 0;
    DWORD outputTransferSize = sizeof(outputTransferFlag);
    DWORD outputTransferType = 0;
    if (RegQueryValueExW(key, L"OutputTransferFunc", nullptr, &outputTransferType,
                          reinterpret_cast<BYTE*>(&outputTransferFlag), &outputTransferSize) == ERROR_SUCCESS
        && outputTransferType == REG_DWORD)
    {
        _outputTransferFunc709 = (outputTransferFlag != 0);
    }

    // This one DOES change actual pixel math, unlike OutputTransferFunc above — it
    // gates whether the GPU shader decodes HLG (EOTF + system-gamma OOTF) before
    // handing RGB to the 3D LUT, versus feeding the LUT raw un-decoded code values
    // (current/previous behavior). Untested against these specific .3dlut files, so
    // it's off by default; flip and compare.
    //   HKCU\Software\LutHdrFilter\ApplyHlgOOTF (REG_DWORD): 0 = off (default), 1 = on
    DWORD ootfFlag = 0;
    DWORD ootfSize = sizeof(ootfFlag);
    DWORD ootfType = 0;
    if (RegQueryValueExW(key, L"ApplyHlgOOTF", nullptr, &ootfType,
                          reinterpret_cast<BYTE*>(&ootfFlag), &ootfSize) == ERROR_SUCCESS
        && ootfType == REG_DWORD)
    {
        _applyHlgOotf = (ootfFlag != 0);
    }

    RegCloseKey(key);
}

void CLutHdrFilter::LoadHotkeysFromRegistry()
{
    // Default is unbound (vk=0) for every hotkey - the filter never grabs a key
    // combo the user didn't explicitly set on the property page.
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LutHdrFilter", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    auto readHotkey = [&](const wchar_t* name, HotkeyBinding& binding)
    {
        DWORD packed = 0, size = sizeof(packed), type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&packed), &size) == ERROR_SUCCESS
            && type == REG_DWORD)
        {
            UnpackHotkey(packed, binding.vk, binding.modifiers);
        }
    };

    readHotkey(L"HotkeyToggleEnable", _hkToggleEnable);
    readHotkey(L"HotkeyForceAuto", _hkForceAuto);
    readHotkey(L"HotkeyForce709", _hkForce709);
    readHotkey(L"HotkeyForce2020", _hkForce2020);
    readHotkey(L"HotkeyForceDCIP3", _hkForceDCIP3);

    RegCloseKey(key);
}

void CLutHdrFilter::OnHotkeyFired(int id)
{
    // Runs on the hotkey thread (see HotkeyManager) - only touches the atomics
    // Transform() reads, never anything that would need a lock.
    if (id == _hkToggleEnable.id)
    {
        const bool nowBypassed = !_hotkeyBypass.load(std::memory_order_relaxed);
        _hotkeyBypass.store(nowBypassed, std::memory_order_relaxed);
        LutHdrTrace(L"Hotkey: processing %s", nowBypassed ? L"DISABLED (passthrough)" : L"ENABLED");
    }
    else if (id == _hkForceAuto.id)
    {
        _hotkeyLutOverride.store(-1, std::memory_order_relaxed);
        LutHdrTrace(L"Hotkey: LUT selection = Auto (detected color space)");
    }
    else if (id == _hkForce709.id)
    {
        _hotkeyLutOverride.store(0, std::memory_order_relaxed);
        LutHdrTrace(L"Hotkey: LUT selection forced to BT.709");
    }
    else if (id == _hkForce2020.id)
    {
        _hotkeyLutOverride.store(1, std::memory_order_relaxed);
        LutHdrTrace(L"Hotkey: LUT selection forced to BT.2020");
    }
    else if (id == _hkForceDCIP3.id)
    {
        _hotkeyLutOverride.store(2, std::memory_order_relaxed);
        LutHdrTrace(L"Hotkey: LUT selection forced to DCI-P3");
    }
}

bool CLutHdrFilter::ShouldProcessCurrentVideo() const
{
    return ComputeShouldProcess(_colorInfo, _format, _width, _forceP010AsHlg,
                                 _pathLut709, _pathLut2020, _pathLutDCIP3);
}

void CLutHdrFilter::EnsureLutLoaded(SourceColorSpace space)
{
    // Deferred from the constructor - see the comment there. This only runs when
    // ShouldProcessCurrentVideo() is already true, so we're not paying the device +
    // shader-compile cost for content that will just pass through untouched.
    if (!_gpuEngine.IsInitialized())
    {
        bool gpuOk = _gpuEngine.Initialize();
        LutHdrTrace(L"EnsureLutLoaded: deferred gpuEngine.Initialize()=%s", gpuOk ? L"OK" : L"FAILED");
        if (!gpuOk)
            return;
    }

    // Load every configured LUT up front, not just the one matching `space`.
    // The hotkey LUT override (see Transform()) needs to be able to switch to
    // any of the three instantly, mid-playback, without a synchronous
    // LoadLutDirect stalling the streaming thread the first time it's selected.
    // LoadLutDirect already early-returns cheaply if a path is already cached
    // (see D3D11LutEngine.cpp), so calling all three on every SetMediaType is fine.
    if (!_pathLut709.empty())
        _gpuEngine.LoadLutDirect(SourceColorSpace::BT709, _pathLut709);
    if (!_pathLut2020.empty())
        _gpuEngine.LoadLutDirect(SourceColorSpace::BT2020, _pathLut2020);
    if (!_pathLutDCIP3.empty())
        _gpuEngine.LoadLutDirect(SourceColorSpace::DCI_P3, _pathLutDCIP3);

    (void)space; // no longer used to filter which LUT(s) get loaded, see above
}

const Lut3D& CLutHdrFilter::SelectLut(SourceColorSpace space) const
{
    switch (space)
    {
    case SourceColorSpace::BT2020: return _lut2020;
    case SourceColorSpace::DCI_P3: return _lutDciP3;
    case SourceColorSpace::BT709:
    default:                       return _lut709;
    }
}

PixelFormat CLutHdrFilter::DetectPixelFormat(const GUID& subtype)
{
    if (subtype == MEDIASUBTYPE_RGB32 || subtype == MEDIASUBTYPE_ARGB32)
        return PixelFormat::RGB32;
    if (IsFourCCSubtype(subtype, MakeFourCC('N', 'V', '1', '2')))
        return PixelFormat::NV12;
    if (IsFourCCSubtype(subtype, MakeFourCC('P', '0', '1', '0')))
        return PixelFormat::P010;
    return PixelFormat::Unknown;
}

HRESULT CLutHdrFilter::CheckInputType(const CMediaType* mtIn)
{
    if (mtIn->majortype != MEDIATYPE_Video)
        return VFW_E_TYPE_NOT_ACCEPTED;

    const PixelFormat format = DetectPixelFormat(mtIn->subtype);
    if (format == PixelFormat::Unknown)
        return VFW_E_TYPE_NOT_ACCEPTED;

    // Decode color info straight from the proposed type - this is the exact same
    // VIDEOINFOHEADER2 block SetMediaType decodes later, just read one step earlier,
    // before we've committed to the connection. That lets us reject anything this
    // filter will never actually process (i.e. everything that isn't signaled HLG,
    // with a matching LUT path configured) right here, so it's never spliced into
    // the graph at all for ordinary SDR content: no per-frame passthrough copy, no
    // stride/coded-height guessing on that content's behalf, nothing. MPC-BE's
    // forced "Insert" will just fail to insert us for that connection and playback
    // proceeds straight from decoder to renderer, exactly as if the filter weren't
    // installed.
    DetectedColorInfo info{};
    int width = 0;

    if (mtIn->formattype == FORMAT_VideoInfo2 && mtIn->cbFormat >= sizeof(VIDEOINFOHEADER2))
    {
        const auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(mtIn->pbFormat);
        info = DecodeColorInfo(vih2);
        width = vih2->bmiHeader.biWidth;
    }
    else if (mtIn->formattype == FORMAT_VideoInfo && mtIn->cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(mtIn->pbFormat);
        width = vih->bmiHeader.biWidth;
        // FORMAT_VideoInfo carries no color-info block at all, so `info` stays
        // "not present" - only the ForceP010AsHLG override (handled inside
        // ComputeShouldProcess) can make this connection acceptable.
    }
    else
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    if (!ComputeShouldProcess(info, format, width, _forceP010AsHlg,
                              _pathLut709, _pathLut2020, _pathLutDCIP3))
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    return S_OK;
}

HRESULT CLutHdrFilter::CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut)
{
    if (FAILED(CheckInputType(mtIn)))
        return VFW_E_TYPE_NOT_ACCEPTED;

    if (mtIn->majortype != mtOut->majortype || mtIn->subtype != mtOut->subtype)
        return VFW_E_TYPE_NOT_ACCEPTED;

    return S_OK;
}

HRESULT CLutHdrFilter::DecideBufferSize(IMemAllocator* alloc, ALLOCATOR_PROPERTIES* props)
{
    if (!m_pInput->IsConnected())
        return E_UNEXPECTED;

    long bufferSize = 0;
    switch (_format)
    {
    case PixelFormat::RGB32:
        bufferSize = static_cast<long>(_strideBytes) * _height;
        break;
    case PixelFormat::NV12:
    case PixelFormat::P010:
    {
        const int uvHeight = (_height + 1) / 2;
        bufferSize = static_cast<long>(_strideBytes) * _height +
                     static_cast<long>(_strideBytes) * uvHeight;
        break;
    }
    default:
        return E_UNEXPECTED;
    }

    ALLOCATOR_PROPERTIES request = *props;
    // Was std::max(1L, props->cBuffers). The downstream pin asks for 0 here, so that
    // resolved to exactly ONE ~25 MB buffer (see the DecideBufferSize trace: requested
    // cBuf=1, actual cBuf=1). With a single output buffer, CTransformFilter::Receive
    // blocks in GetDeliveryBuffer() until the renderer has released the previous frame,
    // so decode, this filter, and the renderer can never overlap - each frame pays the
    // SUM of all three instead of the slowest one. That is fine at 30fps and falls over
    // at 4K60. A few spare buffers let the stages run concurrently.
    request.cBuffers = std::max(4L, props->cBuffers);
    request.cbBuffer = std::max(bufferSize, props->cbBuffer);
    request.cbAlign = std::max(1L, props->cbAlign);

    ALLOCATOR_PROPERTIES actual{};
    HRESULT hr = alloc->SetProperties(&request, &actual);

    LutHdrTrace(L"DecideBufferSize: fmt=%s computed=%ld propsIn(cBuf=%ld,cbBuf=%ld,cbAlign=%ld) requested(cBuf=%ld,cbBuf=%ld,cbAlign=%ld) actual(cBuf=%ld,cbBuf=%ld,cbAlign=%ld) hr=0x%08X",
           ToStr(_format), bufferSize,
           props->cBuffers, props->cbBuffer, props->cbAlign,
           request.cBuffers, request.cbBuffer, request.cbAlign,
           actual.cBuffers, actual.cbBuffer, actual.cbAlign, (unsigned int)hr);

    if (FAILED(hr))
        return hr;

    return (actual.cbBuffer < request.cbBuffer) ? E_FAIL : S_OK;
}

HRESULT CLutHdrFilter::GetMediaType(int iPosition, CMediaType* mtOut)
{
    if (iPosition < 0)
        return E_INVALIDARG;
    if (iPosition > 0)
        return VFW_S_NO_MORE_ITEMS;

    *mtOut = m_pInput->CurrentMediaType();

    bool willProcess = ShouldProcessCurrentVideo();
    LutHdrTrace(L"GetMediaType: willProcess=%d (rewriting output color info=%s)", willProcess ? 1 : 0, willProcess ? L"yes" : L"no");

    if (willProcess)
    {
        if (mtOut->formattype == FORMAT_VideoInfo2 && mtOut->cbFormat >= sizeof(VIDEOINFOHEADER2))
        {
            auto* vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(mtOut->pbFormat);
            auto* ext = reinterpret_cast<PackedExtendedFormat*>(&vih2->dwControlFlags);

            ext->VideoPrimaries = MFPrimaries::BT709;
            // Toggle via HKCU\Software\LutHdrFilter\OutputTransferFunc (see LoadConfigFromRegistry).
            ext->VideoTransferFunction = _outputTransferFunc709 ? MFTransferFunc::Func709 : MFTransferFunc::Func22;
            ext->VideoTransferMatrix = MFMatrix::BT709;

            vih2->dwControlFlags |= AMCONTROL_COLORINFO_PRESENT;
        }
    }

    return S_OK;
}

HRESULT CLutHdrFilter::SetMediaType(PIN_DIRECTION direction, const CMediaType* mt)
{
    HRESULT hr = CTransformFilter::SetMediaType(direction, mt);
    if (FAILED(hr))
        return hr;

    if (direction != PINDIR_INPUT)
        return S_OK;

    _format = DetectPixelFormat(mt->subtype);

    const VIDEOINFOHEADER2* vih2 = nullptr;
    if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2))
    {
        vih2 = reinterpret_cast<VIDEOINFOHEADER2*>(mt->pbFormat);
        _width = vih2->bmiHeader.biWidth;
        _height = abs(vih2->bmiHeader.biHeight);
    }
    else if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
        _width = vih->bmiHeader.biWidth;
        _height = abs(vih->bmiHeader.biHeight);
    }
    else
    {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    // FIX FOR 720p BLACK SCREEN: Correct stride calculation per format
    switch (_format)
    {
    case PixelFormat::RGB32:
        _strideBytes = ((_width * 32 + 31) / 32) * 4;
        break;
    case PixelFormat::NV12:
        _strideBytes = _width; // 720 bytes for 720p NV12
        break;
    case PixelFormat::P010:
        _strideBytes = _width * 2; // 7680 bytes for 4K P010
        break;
    default:
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    _colorInfo = vih2 ? DecodeColorInfo(vih2) : DetectedColorInfo{};

    // NOTE: we deliberately do NOT guess HDR from resolution/pixel-format alone
    // anymore. P010 4K with no signaled color info is just as likely to be a
    // perfectly ordinary SDR file (many exporters/phones mux P010 without ever
    // setting AMCONTROL_COLORINFO_PRESENT) as it is to be real HLG HDR. Guessing
    // wrong here means: (a) the filter processes video it should be passing
    // through untouched, and (b) it feeds the wrong YCbCr matrix (BT.2020
    // instead of the source's real BT.709) into decode, which is what produces
    // the hue-rotated/blown-out output (navy -> green, skin -> violet, etc.).
    // Default behavior is now: no signaled color info => treat as SDR => bypass.
    // If you genuinely have unflagged 4K HLG masters and want them forced
    // through the HDR path, opt in explicitly via the registry:
    //   HKCU\Software\LutHdrFilter\ForceP010AsHLG (REG_DWORD) = 1
    if (!_colorInfo.present && _format == PixelFormat::P010 && _width >= 3840 && _forceP010AsHlg)
    {
        _colorInfo.space = SourceColorSpace::BT2020;
        _colorInfo.matrix = SourceMatrix::BT2020;
        _colorInfo.transfer = SourceTransfer::HLG;
        _colorInfo.range = SourceRange::Limited;
        _colorInfo.present = true;
    }

    if (ShouldProcessCurrentVideo())
    {
        EnsureLutLoaded(_colorInfo.space);
    }

    LutHdrTrace(L"SetMediaType(INPUT): fmt=%s %dx%d stride=%d | present=%d space=%s transfer=%s matrix=%s range=%s | willProcess=%d",
           ToStr(_format), _width, _height, _strideBytes,
           _colorInfo.present ? 1 : 0, ToStr(_colorInfo.space), ToStr(_colorInfo.transfer),
           ToStr(_colorInfo.matrix), ToStr(_colorInfo.range), ShouldProcessCurrentVideo() ? 1 : 0);

    return S_OK;
}

HRESULT CLutHdrFilter::Transform(IMediaSample* in, IMediaSample* out)
{
    static long s_frameCount = 0;
    const bool logThisFrame = (s_frameCount < 10);
    ++s_frameCount;

    BYTE* srcPtr = nullptr;
    BYTE* dstPtr = nullptr;
    HRESULT hr = in->GetPointer(&srcPtr);
    if (FAILED(hr))
    {
        LutHdrTrace(L"Transform: in->GetPointer FAILED hr=0x%08X", (unsigned int)hr);
        return hr;
    }
    hr = out->GetPointer(&dstPtr);
    if (FAILED(hr))
    {
        LutHdrTrace(L"Transform: out->GetPointer FAILED hr=0x%08X", (unsigned int)hr);
        return hr;
    }

    long inActualLen = in->GetActualDataLength();
    long inSize = in->GetSize();
    long outSize = out->GetSize();
    long expected = static_cast<long>(_strideBytes) * _height * 3 / 2;

    long len = inActualLen;
    if (len <= 0) len = inSize;
    if (len <= 0) len = expected;

    // Derive each side's REAL row stride *and* real coded height (where its UV plane
    // actually starts) independently from its own buffer size, rather than assuming
    // either side is tightly packed at _width/_height. This is what was silently
    // corrupting frames: a source whose decode surface pads coded height (e.g.
    // 750x1000 -> a very plausible real 1008) has its chroma plane start 8 rows
    // later than _height would suggest, so any offset computed from _height alone
    // reads stale/garbage bytes as chroma — independent of, and in addition to, the
    // row-stride padding this used to guard against.
    int srcStride = 0, dstStride = 0, srcCodedHeight = _height, dstCodedHeight = _height;
    if (_format == PixelFormat::RGB32)
    {
        srcStride = DeriveRgb32Stride(inSize > 0 ? inSize : len, _width, _height);
        dstStride = DeriveRgb32Stride(outSize > 0 ? outSize : len, _width, _height);
    }
    else
    {
        const PlaneLayout srcLayout = DeriveNV12P010Layout(inSize > 0 ? inSize : len, _width, _height, _format);
        const PlaneLayout dstLayout = DeriveNV12P010Layout(outSize > 0 ? outSize : len, _width, _height, _format);
        srcStride = srcLayout.stride;
        dstStride = dstLayout.stride;
        srcCodedHeight = srcLayout.codedHeight;
        dstCodedHeight = dstLayout.codedHeight;
    }

    const long outActualLen = (dstStride > 0)
        ? ((_format == PixelFormat::RGB32)
            ? static_cast<long>(dstStride) * _height
            : static_cast<long>(dstStride) * dstCodedHeight + static_cast<long>(dstStride) * ((dstCodedHeight + 1) / 2))
        : len;
    out->SetActualDataLength(outActualLen);

    if (logThisFrame)
    {
        LutHdrTrace(L"Transform[%ld]: fmt=%s %dx%d willProcess=%d | in.ActualLen=%ld in.Size=%ld out.Size=%ld srcStride=%d dstStride=%d srcCodedHeight=%d dstCodedHeight=%d outActualLen=%ld",
               s_frameCount, ToStr(_format), _width, _height, ShouldProcessCurrentVideo() ? 1 : 0,
               inActualLen, inSize, outSize, srcStride, dstStride, srcCodedHeight, dstCodedHeight, outActualLen);
        if (srcStride != dstStride || srcCodedHeight != dstCodedHeight)
        {
            LutHdrTrace(L"Transform[%ld]: *** layout mismatch detected: src(stride=%d,codedHeight=%d) dst(stride=%d,codedHeight=%d) — using plane-aware copy to avoid corrupting the frame ***",
                   s_frameCount, srcStride, srcCodedHeight, dstStride, dstCodedHeight);
        }
        if (srcCodedHeight != _height)
        {
            LutHdrTrace(L"Transform[%ld]: *** source coded height (%d) differs from declared height (%d) — this is the padding that used to shift chroma reads ***",
                   s_frameCount, srcCodedHeight, _height);
        }
    }

    // Hotkey "disable processing" (IDC_HOTKEY_TOGGLE_ENABLE) forces passthrough
    // regardless of what ShouldProcessCurrentVideo() would otherwise decide - this
    // is the runtime on/off switch, separate from the stream-driven gate below.
    const bool hotkeyBypassed = _hotkeyBypass.load(std::memory_order_relaxed);

    // EXACT WORKING PASSTHROUGH: Guarantees SDR video is never black, now stride- and
    // coded-height-aware.
    if (hotkeyBypassed || !ShouldProcessCurrentVideo())
    {
        if (logThisFrame && hotkeyBypassed)
            LutHdrTrace(L"Transform[%ld]: hotkey bypass active, passthrough", s_frameCount);
        CopyFrameRespectingStride(srcPtr, dstPtr, srcStride, dstStride, srcCodedHeight, dstCodedHeight, _width, _height, _format);
        out->SetSyncPoint(TRUE);
        return S_OK;
    }

    // Direct3D 11 GPU execution for P010 HLG
    if (_format == PixelFormat::P010)
    {
        // Hotkey LUT override (IDC_HOTKEY_FORCE_*): -1 = leave the detected color
        // space alone, 0/1/2 = force BT.709/BT.2020/DCI-P3 regardless of what the
        // stream signaled. D3D11LutEngine::ProcessFrameP010 picks its active LUT
        // purely from colorInfo.space (see D3D11LutEngine.cpp), so overriding it
        // here is enough - no GPU-engine changes needed. The matrix/range fields
        // are left as detected since those describe the actual bitstream, not
        // which LUT to apply to it.
        DetectedColorInfo effectiveColorInfo = _colorInfo;
        switch (_hotkeyLutOverride.load(std::memory_order_relaxed))
        {
        case 0: effectiveColorInfo.space = SourceColorSpace::BT709;  break;
        case 1: effectiveColorInfo.space = SourceColorSpace::BT2020; break;
        case 2: effectiveColorInfo.space = SourceColorSpace::DCI_P3; break;
        default: break; // -1 = Auto, keep detected space
        }

        bool gpuOk = _gpuEngine.ProcessFrameP010(srcPtr, dstPtr, _width, _height, srcStride, srcCodedHeight, dstStride, dstCodedHeight, effectiveColorInfo);
        if (logThisFrame)
            LutHdrTrace(L"Transform[%ld]: GPU ProcessFrameP010 returned %s", s_frameCount, gpuOk ? L"true" : L"false (falling back to memcpy passthrough)");
        if (gpuOk)
        {
            out->SetSyncPoint(TRUE);
            return S_OK;
        }
    }

    CopyFrameRespectingStride(srcPtr, dstPtr, srcStride, dstStride, srcCodedHeight, dstCodedHeight, _width, _height, _format);
    out->SetSyncPoint(TRUE);
    return S_OK;
}