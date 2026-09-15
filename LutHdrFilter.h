#pragma once

#include <streams.h>
#include <dvdmedia.h>
#include <ocidl.h>
#include <string>
#include <atomic>
#include "Lut3D.h"
#include "ColorSpace.h"
#include "ColorSpaceDirectShow.h" // DecodeColorInfo(VIDEOINFOHEADER2*) - DirectShow-only, split out for the VLC plugin's sake
#include "PixelOps.h"
#include "YuvOps.h"
#include "D3D11LutEngine.h"
#include "HotkeyManager.h"

DEFINE_GUID(CLSID_LutHdrFilter,
    0x7e3f1a20, 0x9b4c, 0x4e0f, 0x9c, 0x2a, 0x1f, 0x8b, 0x6d, 0x2a, 0x00, 0x01);

enum class PixelFormat
{
    Unknown,
    RGB32,
    NV12,
    P010,
};

class CLutHdrFilter : public CTransformFilter, public ISpecifyPropertyPages
{
public:
    static CUnknown* WINAPI CreateInstance(LPUNKNOWN punk, HRESULT* phr);

    // Required here (not just inherited from CTransformFilter) because adding
    // ISpecifyPropertyPages as a second base brings in its own, separate IUnknown
    // path. DECLARE_IUNKNOWN puts one concrete QueryInterface/AddRef/Release right
    // on this class, which is what lets a single override satisfy both inherited
    // IUnknown vtables - without it the compiler sees ISpecifyPropertyPages's
    // QueryInterface/AddRef/Release as still-abstract and the class won't instantiate
    // (exactly the C2259 error this fixes).
    DECLARE_IUNKNOWN;
    STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void** ppv) override;

    // ISpecifyPropertyPages
    STDMETHODIMP GetPages(CAUUID* pPages) override;

    HRESULT CheckInputType(const CMediaType* mtIn) override;
    HRESULT CheckTransform(const CMediaType* mtIn, const CMediaType* mtOut) override;
    HRESULT DecideBufferSize(IMemAllocator* alloc, ALLOCATOR_PROPERTIES* props) override;
    HRESULT GetMediaType(int iPosition, CMediaType* mtOut) override;
    HRESULT SetMediaType(PIN_DIRECTION direction, const CMediaType* mt) override;
    HRESULT Transform(IMediaSample* in, IMediaSample* out) override;

private:
    CLutHdrFilter(LPUNKNOWN punk, HRESULT* phr);

    void LoadConfigFromRegistry();
    bool ShouldProcessCurrentVideo() const; // Passthrough check
    void EnsureLutLoaded(SourceColorSpace space);
    const Lut3D& SelectLut(SourceColorSpace space) const;
    static PixelFormat DetectPixelFormat(const GUID& subtype);

    // --- Live hotkeys (enable/disable + LUT switching while a video plays) ----
    // Bindings are read once at construction (same "takes effect next time you
    // open a file" rule as the rest of the config - see LutHdrPropPage.h). Once
    // registered, they act on THIS running filter instance for as long as it's
    // alive, which is what makes them work mid-playback.
    void LoadHotkeysFromRegistry();
    void OnHotkeyFired(int id); // runs on the hotkey thread, not the streaming thread

    HotkeyManager _hotkeys;
    HotkeyBinding _hkToggleEnable{ 1, 0, 0 };
    HotkeyBinding _hkForceAuto{ 2, 0, 0 };
    HotkeyBinding _hkForce709{ 3, 0, 0 };
    HotkeyBinding _hkForce2020{ 4, 0, 0 };
    HotkeyBinding _hkForceDCIP3{ 5, 0, 0 };

    // Written only from the hotkey thread (OnHotkeyFired), read from the
    // streaming thread inside Transform() - plain atomics rather than a lock
    // since each is an independent flag/enum, not composite state.
    std::atomic<bool> _hotkeyBypass{ false };   // true = force passthrough
    std::atomic<int>  _hotkeyLutOverride{ -1 }; // -1=Auto, 0=BT709, 1=BT2020, 2=DCI_P3

    int  _width = 0;
    int  _height = 0;
    int  _strideBytes = 0;
    PixelFormat _format = PixelFormat::Unknown;

    DetectedColorInfo _colorInfo;

    std::wstring _pathLut709;
    std::wstring _pathLut2020;
    std::wstring _pathLutDCIP3;
    bool _lut709Loaded = false;
    bool _lut2020Loaded = false;
    bool _lutDCIP3Loaded = false;

    // Off by default: see the comment above the heuristic in SetMediaType().
    // Only set true (via HKCU\Software\LutHdrFilter\ForceP010AsHLG=1) if you
    // knowingly have unflagged 4K HLG source and want it force-processed.
    bool _forceP010AsHlg = false;

    // false = tag output as Func22 (pure 2.2 gamma, current default), true = tag as
    // Func709 (BT.709 OETF). See LoadConfigFromRegistry / OutputTransferFunc.
    bool _outputTransferFunc709 = false;

    // false = feed the LUT raw/un-decoded RGB (current default), true = decode HLG
    // (EOTF+OOTF) before the LUT. See LoadConfigFromRegistry / ApplyHlgOOTF.
    bool _applyHlgOotf = false;

    Lut3D _lut709;
    Lut3D _lut2020;
    Lut3D _lutDciP3;

    ToneMapSettings _toneMap;
    D3D11LutEngine  _gpuEngine;
};