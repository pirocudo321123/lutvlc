#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "ColorSpace.h"
#include "Lut3D.h"

using Microsoft::WRL::ComPtr;

class D3D11LutEngine
{
public:
    D3D11LutEngine();
    ~D3D11LutEngine();

    bool Initialize();
    bool IsInitialized() const { return m_initialized; }
    bool LoadLutDirect(SourceColorSpace space, const std::wstring& path);
    void SetApplyHlgOotf(bool apply) { m_applyHlgOotf = apply; }
    // srcCodedHeight/dstCodedHeight: row index where each side's real UV plane
    // starts (can differ from `height` when either side's decode/render surface
    // pads coded height beyond the stream's declared height — see the layout
    // solver in LutHdrFilter.cpp). Pass `height` for both when the caller knows
    // neither side is padded.
    bool ProcessFrameP010(
        const uint8_t* src,
        uint8_t* dst,
        int width,
        int height,
        int srcStrideBytes,
        int srcCodedHeight,
        int dstStrideBytes,
        int dstCodedHeight,
        const DetectedColorInfo& colorInfo);

private:
    bool CompileComputeShader();
    bool EnsureTextures(int width, int height);

    ComPtr<ID3D11Device>        m_device;
    ComPtr<ID3D11DeviceContext>   m_context;
    ComPtr<ID3D11ComputeShader>   m_computeShader;
    ComPtr<ID3D11SamplerState>    m_sampler;
    ComPtr<ID3D11Buffer>          m_constantBuffer;

    // 3D LUT shader resource views and cached paths
    ComPtr<ID3D11ShaderResourceView> m_srvLut709;
    ComPtr<ID3D11ShaderResourceView> m_srvLut2020;
    ComPtr<ID3D11ShaderResourceView> m_srvLutDCIP3;
    ComPtr<ID3D11ShaderResourceView> m_srvDefaultLut;

    std::wstring m_cachedPath709;
    std::wstring m_cachedPath2020;
    std::wstring m_cachedPathDCIP3;

    int m_lutSize2020  = 256;
    int m_lutSize709   = 256;
    int m_lutSizeDCIP3 = 256;

    int m_texWidth  = 0;
    int m_texHeight = 0;

    // Input textures
    ComPtr<ID3D11Texture2D>          m_texY_In;
    ComPtr<ID3D11ShaderResourceView> m_srvY_In;

    ComPtr<ID3D11Texture2D>          m_texUV_In;
    ComPtr<ID3D11ShaderResourceView> m_srvUV_In;

    // Output UAV textures
    ComPtr<ID3D11Texture2D>           m_texY_Out;
    ComPtr<ID3D11UnorderedAccessView> m_uavY_Out;

    ComPtr<ID3D11Texture2D>           m_texUV_Out;
    ComPtr<ID3D11UnorderedAccessView> m_uavUV_Out;

    // CPU staging readback textures
    ComPtr<ID3D11Texture2D>          m_texY_Staging;
    ComPtr<ID3D11Texture2D>          m_texUV_Staging;

    bool m_initialized = false;
    bool m_applyHlgOotf = false;
};