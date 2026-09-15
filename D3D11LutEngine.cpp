// D3D11LutEngine.cpp
#include "D3D11LutEngine.h"
#include "YuvOps.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <sstream>
#include <ppl.h>
#include <vector>
#include <cstdint>
#include <mutex>
#include <map>

namespace
{
    // Same DebugView-visible tracing as LutHdrFilter.cpp (filter on "LUTHDR:"),
    // duplicated locally so LUT load failures here are no longer silent.
    void LutHdrTraceGpu(const wchar_t* fmt, ...)
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

    struct ShaderConstants
    {
        float in_kr;
        float in_kb;
        float out_kr;
        float out_kb;
        uint32_t width;
        uint32_t height;
        uint32_t is_limited;
        float lut_size;
        uint32_t apply_ootf;
        float _pad0, _pad1, _pad2; // keep 16-byte aligned total size
    };

    const char* g_computeShaderCode = R"(
        cbuffer Params : register(b0)
        {
            float in_kr;
            float in_kb;
            float out_kr;
            float out_kb;
            uint width;
            uint height;
            uint is_limited;
            float lut_size;
            uint apply_ootf;
            float _pad0, _pad1, _pad2;
        };

        Texture2D<uint> texY_In : register(t0);
        Texture2D<uint2> texUV_In : register(t1);
        Texture3D<float4> lut3D : register(t2);
        SamplerState linearSampler : register(s0);

        RWTexture2D<uint> texY_Out : register(u0);
        RWTexture2D<uint2> texUV_Out : register(u1);

        float3 YCbCrToRgb(float y, float cb, float cr, float kr, float kb)
        {
            float r = y + 2.0f * (1.0f - kr) * cr;
            float b = y + 2.0f * (1.0f - kb) * cb;
            float g = (y - kr * r - kb * b) / (1.0f - kr - kb);
            return float3(r, g, b);
        }

        void RgbToYCbCr(float3 rgb, float kr, float kb, out float y, out float cb, out float cr)
        {
            y = kr * rgb.r + (1.0f - kr - kb) * rgb.g + kb * rgb.b;
            cb = (rgb.b - y) / (2.0f * (1.0f - kb));
            cr = (rgb.r - y) / (2.0f * (1.0f - kr));
        }

        float2 DecodeChroma10(uint2 raw)
        {
            uint cb10 = raw.x >> 6;
            uint cr10 = raw.y >> 6;
            float cb = (is_limited != 0) ? ((float(cb10) - 512.0f) / 896.0f) : ((float(cb10) - 512.0f) / 1023.0f);
            float cr = (is_limited != 0) ? ((float(cr10) - 512.0f) / 896.0f) : ((float(cr10) - 512.0f) / 1023.0f);
            return float2(cb, cr);
        }

        // ARIB STD-B67 / BT.2100 HLG inverse OETF: HLG code value [0,1] -> scene-linear,
        // nominal reference white ~= 1.0. Followed by the BT.2100 system-gamma OOTF
        // (per-channel simplification, same caveat as the CPU stub this replaces: exact
        // spec applies one factor derived from luminance, not per-channel — fine for
        // near-neutral content, can shift saturated highlights slightly). Only runs when
        // apply_ootf != 0 (HKCU\Software\LutHdrFilter\ApplyHlgOOTF), since it's untested
        // against these specific LUT files and may or may not be what they expect.
        float HlgEotf(float e)
        {
            const float a = 0.17883277f, b = 0.28466892f, c = 0.55991073f; // c = 1 - 4a
            if (e <= 0.5f)
                return (e * e) / 3.0f;
            return (exp((e - c) / a) + b) / 12.0f;
        }

        float3 ApplyHlgOotf(float3 rgb)
        {
            float3 lin = float3(HlgEotf(rgb.r), HlgEotf(rgb.g), HlgEotf(rgb.b));
            // BT.2100 nominal system gamma for a 1000-nit reference display.
            const float gamma = 1.2f;
            float ys = 0.2627f * lin.r + 0.6780f * lin.g + 0.0593f * lin.b;
            float factor = pow(max(ys, 0.0001f), gamma - 1.0f);
            return saturate(lin * factor);
        }

        [numthreads(16, 16, 1)]
        void CSMain(uint3 id : SV_DispatchThreadID)
        {
            uint bx = id.x * 2;
            uint by = id.y * 2;
            if (bx >= width || by >= height) return;

            // 4:2:0 chroma is one Cb/Cr sample per 2x2 luma block. Sampling ONLY the
            // current block (nearest-neighbor) and sharing it flat across all 4 pixels
            // is what produced the cyan fringing at hard edges: at a sharp luma
            // transition, the shared chroma doesn't match the actual edge and the
            // mismatch gets carried straight through the LUT. Instead, bilinearly blend
            // between this block and its right/below/diagonal neighbors, so chroma
            // transitions smoothly across block boundaries like it should.
            uint2 uvMax = uint2((width + 1) / 2 - 1, (height + 1) / 2 - 1);
            uint2 c00 = id.xy;
            uint2 c10 = uint2(min(id.x + 1, uvMax.x), id.y);
            uint2 c01 = uint2(id.x, min(id.y + 1, uvMax.y));
            uint2 c11 = uint2(min(id.x + 1, uvMax.x), min(id.y + 1, uvMax.y));

            float2 cc00 = DecodeChroma10(texUV_In[c00]);
            float2 cc10 = DecodeChroma10(texUV_In[c10]);
            float2 cc01 = DecodeChroma10(texUV_In[c01]);
            float2 cc11 = DecodeChroma10(texUV_In[c11]);

            float cbSum = 0.0f;
            float crSum = 0.0f;
            int count = 0;

            float yMin = (is_limited != 0) ? 64.0f : 0.0f;
            float yMax = (is_limited != 0) ? 940.0f : 1023.0f;
            float cMin = (is_limited != 0) ? 64.0f : 0.0f;
            float cMax = (is_limited != 0) ? 960.0f : 1023.0f;

            [unroll]
            for (uint dy = 0; dy < 2; ++dy)
            {
                [unroll]
                for (uint dx = 0; dx < 2; ++dx)
                {
                    uint px = bx + dx;
                    uint py = by + dy;
                    if (px < width && py < height)
                    {
                        // dx/dy==0 is co-sited with this block (weight 0); ==1 blends
                        // halfway toward the next block, approximating true sample
                        // position without needing exact chroma-siting metadata.
                        float fx = dx * 0.5f;
                        float fy = dy * 0.5f;
                        float2 cTop = lerp(cc00, cc10, fx);
                        float2 cBot = lerp(cc01, cc11, fx);
                        float2 c = lerp(cTop, cBot, fy);
                        float cb_norm = c.x;
                        float cr_norm = c.y;

                        uint rawY = texY_In[uint2(px, py)];
                        uint y10 = rawY >> 6;
                        float y_norm = (is_limited != 0) ? ((float(y10) - 64.0f) / 876.0f) : (float(y10) / 1023.0f);

                        float3 rgb = YCbCrToRgb(y_norm, cb_norm, cr_norm, in_kr, in_kb);
                        rgb = saturate(rgb);
                        if (apply_ootf != 0)
                            rgb = ApplyHlgOotf(rgb);

                        float3 processed = lut3D.SampleLevel(linearSampler, rgb, 0).rgb;
                        processed = saturate(processed);

                        float outY, outCb, outCr;
                        RgbToYCbCr(processed, out_kr, out_kb, outY, outCb, outCr);

                        float yVal = (is_limited != 0) ? (outY * 876.0f + 64.0f) : (outY * 1023.0f);
                        uint ny10 = uint(clamp(yVal + 0.5f, yMin, yMax));
                        texY_Out[uint2(px, py)] = (ny10 << 6);

                        float cbVal = (is_limited != 0) ? (outCb * 896.0f + 512.0f) : (outCb * 1023.0f + 512.0f);
                        float crVal = (is_limited != 0) ? (outCr * 896.0f + 512.0f) : (outCr * 1023.0f + 512.0f);
                        cbSum += clamp(cbVal, cMin, cMax);
                        crSum += clamp(crVal, cMin, cMax);
                        count++;
                    }
                }
            }

            if (count > 0)
            {
                uint avgCb = uint(clamp(cbSum / count + 0.5f, cMin, cMax));
                uint avgCr = uint(clamp(crSum / count + 0.5f, cMin, cMax));
                texUV_Out[id.xy] = uint2(avgCb << 6, avgCr << 6);
            }
        }
    )";
}

D3D11LutEngine::D3D11LutEngine() {}
D3D11LutEngine::~D3D11LutEngine() {}

namespace
{
    // Compiled-bytecode cache for the compute shader. D3DCompile() on the HLSL
    // source text is the single most expensive step in Initialize() when it does
    // run (it also forces d3dcompiler_47.dll to load into the process) - real
    // enough that it's worth avoiding entirely after the first successful launch.
    // The cache file's own header carries a hash of the shader source, so editing
    // g_computeShaderCode above always invalidates it and forces a clean recompile
    // rather than risking a stale-bytecode mismatch.
    struct ShaderCacheHeader
    {
        uint32_t magic;       // 'LHCS'
        uint32_t sourceHash;
        uint32_t byteCodeSize;
    };
    constexpr uint32_t kShaderCacheMagic = 0x4C484353; // "LHCS"

    uint32_t HashShaderSource(const char* src)
    {
        // FNV-1a - not cryptographic, just needs to change when the source does.
        uint32_t hash = 2166136261u;
        for (const char* p = src; *p; ++p)
        {
            hash ^= static_cast<uint8_t>(*p);
            hash *= 16777619u;
        }
        return hash;
    }

    std::wstring GetShaderCacheFilePath()
    {
        wchar_t tempDir[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, tempDir);
        if (len == 0 || len > MAX_PATH)
            return L"";
        return std::wstring(tempDir) + L"LutHdrFilter_cs.cache";
    }

    // Returns a non-null blob of the cached bytecode on a hit, or nullptr on any
    // miss/mismatch/read failure (all of which just fall back to recompiling).
    bool TryLoadCachedShaderBytecode(const std::wstring& path, uint32_t expectedHash,
                                      std::vector<uint8_t>& outBytes)
    {
        if (path.empty()) return false;

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        ShaderCacheHeader header{};
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return false;
        if (header.magic != kShaderCacheMagic || header.sourceHash != expectedHash) return false;
        if (header.byteCodeSize == 0 || header.byteCodeSize > 16 * 1024 * 1024) return false;

        outBytes.resize(header.byteCodeSize);
        if (!file.read(reinterpret_cast<char*>(outBytes.data()), header.byteCodeSize)) return false;

        return true;
    }

    void SaveShaderCache(const std::wstring& path, uint32_t sourceHash,
                          const void* byteCode, size_t byteCodeSize)
    {
        if (path.empty() || byteCodeSize == 0) return;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return; // best-effort; a failed write just means no cache next time

        ShaderCacheHeader header{ kShaderCacheMagic, sourceHash, static_cast<uint32_t>(byteCodeSize) };
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(byteCode), byteCodeSize);
    }
}


bool D3D11LutEngine::Initialize()
{
    if (m_initialized) return true;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context
    );
    if (FAILED(hr)) return false;

    if (!CompileComputeShader()) return false;

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sampDesc, &m_sampler);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(ShaderConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
    if (FAILED(hr)) return false;

    std::vector<float> identityLut(2 * 2 * 2 * 4);
    for (int b = 0; b < 2; ++b) {
        for (int g = 0; g < 2; ++g) {
            for (int r = 0; r < 2; ++r) {
                int idx = (r + g * 2 + b * 4) * 4;
                identityLut[idx + 0] = float(r);
                identityLut[idx + 1] = float(g);
                identityLut[idx + 2] = float(b);
                identityLut[idx + 3] = 1.0f;
            }
        }
    }

    D3D11_TEXTURE3D_DESC lutDesc{};
    lutDesc.Width = 2;
    lutDesc.Height = 2;
    lutDesc.Depth = 2;
    lutDesc.MipLevels = 1;
    lutDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA lutData{};
    lutData.pSysMem = identityLut.data();
    lutData.SysMemPitch = 2 * sizeof(float) * 4;
    lutData.SysMemSlicePitch = 4 * sizeof(float) * 4;

    ComPtr<ID3D11Texture3D> pTex;
    m_device->CreateTexture3D(&lutDesc, &lutData, &pTex);
    m_device->CreateShaderResourceView(pTex.Get(), nullptr, &m_srvDefaultLut);

    m_initialized = true;
    return true;
}

bool D3D11LutEngine::CompileComputeShader()
{
    const uint32_t sourceHash = HashShaderSource(g_computeShaderCode);
    const std::wstring cachePath = GetShaderCacheFilePath();

    // Fast path: load previously-compiled bytecode straight off disk, skipping
    // D3DCompile (and the d3dcompiler_47.dll load it requires) entirely.
    std::vector<uint8_t> cachedBytes;
    if (TryLoadCachedShaderBytecode(cachePath, sourceHash, cachedBytes))
    {
        HRESULT hrCached = m_device->CreateComputeShader(
            cachedBytes.data(), cachedBytes.size(), nullptr, &m_computeShader);
        if (SUCCEEDED(hrCached))
        {
            LutHdrTraceGpu(L"CompileComputeShader: loaded cached bytecode (%zu bytes), skipped D3DCompile", cachedBytes.size());
            return true;
        }
        // Cached bytes didn't take (e.g. driver/feature-level mismatch) - fall
        // through and recompile from source normally.
        m_computeShader.Reset();
    }

    ComPtr<ID3DBlob> csBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        g_computeShaderCode,
        strlen(g_computeShaderCode),
        nullptr,
        nullptr,
        nullptr,
        "CSMain",
        "cs_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &csBlob,
        &errorBlob
    );

    if (FAILED(hr)) return false;

    hr = m_device->CreateComputeShader(
        csBlob->GetBufferPointer(),
        csBlob->GetBufferSize(),
        nullptr,
        &m_computeShader
    );
    if (FAILED(hr)) return false;

    SaveShaderCache(cachePath, sourceHash, csBlob->GetBufferPointer(), csBlob->GetBufferSize());
    return true;
}

namespace
{
    // Binary .3dlut (3DLT/3DL2 signature): 16-bit RGB triples after a fixed 16KB
    // header (or, for headerless variants, a size inferred from total file size).
    // Matches Lut3D.cpp's Load3DLUT exactly (CPU path, marked as a validated fix
    // there: "Channel 0 is Blue, Channel 1 is Red, Channel 2 is Green"). Output is
    // R-fastest RGBA float, which is exactly the axis order a D3D11 3D texture's
    // SysMemPitch/SysMemSlicePitch expect, so texIdx == i directly, no permutation.
    bool LoadBinary3dlut(const std::wstring& path, int& outSize, std::vector<float>& outData)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;

        char sig[4]{};
        if (!file.read(sig, 4)) return false;
        if (strncmp(sig, "3DLT", 4) != 0 && strncmp(sig, "3DL2", 4) != 0) return false;

        file.seekg(0, std::ios::end);
        size_t fileSize = static_cast<size_t>(file.tellg());

        int size = 0;
        size_t offset = 16384;

        if (fileSize > offset) {
            size_t dataBytes = fileSize - offset;
            if (dataBytes == 256ULL * 256 * 256 * 6) size = 256;
            else if (dataBytes == 65ULL * 65 * 65 * 6) size = 65;
            else if (dataBytes == 33ULL * 33 * 33 * 6) size = 33;
        }
        if (size == 0) {
            if (fileSize >= 100663296) { size = 256; offset = fileSize - (256ULL * 256 * 256 * 6); }
            else if (fileSize >= 1647750) { size = 65; offset = fileSize - (65ULL * 65 * 65 * 6); }
            else {
                LutHdrTraceGpu(L"LoadBinary3dlut: %s has 3DLT/3DL2 signature but unrecognized size (fileSize=%zu)", path.c_str(), fileSize);
                return false;
            }
        }

        file.seekg(offset, std::ios::beg);
        const size_t totalEntries = static_cast<size_t>(size) * size * size;
        std::vector<uint16_t> rawBuffer(totalEntries * 3);
        if (!file.read(reinterpret_cast<char*>(rawBuffer.data()), totalEntries * 3 * sizeof(uint16_t)))
        {
            LutHdrTraceGpu(L"LoadBinary3dlut: %s short read (expected %zu entries)", path.c_str(), totalEntries);
            return false;
        }

        outSize = size;
        outData.resize(totalEntries * 4);
        concurrency::parallel_for(size_t(0), totalEntries, [&](size_t i)
        {
            uint16_t rawB = rawBuffer[i * 3 + 0];
            uint16_t rawR = rawBuffer[i * 3 + 1];
            uint16_t rawG = rawBuffer[i * 3 + 2];

            outData[i * 4 + 0] = float(rawR) / 65535.0f; // Red
            outData[i * 4 + 1] = float(rawG) / 65535.0f; // Green
            outData[i * 4 + 2] = float(rawB) / 65535.0f; // Blue
            outData[i * 4 + 3] = 1.0f;
        });
        return true;
    }

    // Text IRIDAS/Adobe .cube: LUT_3D_SIZE N, then N^3 whitespace-separated "R G B"
    // float triples, red-fastest (per the .cube spec) — the same axis order the
    // binary loader above already produces, so no permutation is needed here either.
    // DOMAIN_MIN/DOMAIN_MAX are read and checked against the default [0,1]; the GPU
    // shader samples the LUT texture directly on a raw [0,1] input with no domain
    // rescale (same limitation the binary path already had), so a non-default domain
    // is logged rather than silently mishandled.
    bool LoadTextCube(const std::wstring& path, int& outSize, std::vector<float>& outData)
    {
        std::ifstream file(path);
        if (!file.is_open()) return false;

        std::vector<float> entries; // flat R,G,B triples, file order
        int size = 0;
        float domainMin[3] = { 0.f, 0.f, 0.f };
        float domainMax[3] = { 1.f, 1.f, 1.f };

        std::string line;
        while (std::getline(file, line))
        {
            size_t start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            if (line.empty() || line[0] == '#') continue;

            std::istringstream iss(line);
            std::string token;
            iss >> token;

            if (token == "TITLE")
            {
                continue;
            }
            else if (token == "LUT_3D_SIZE")
            {
                iss >> size;
            }
            else if (token == "LUT_1D_SIZE")
            {
                LutHdrTraceGpu(L"LoadTextCube: %s is a 1D LUT, not supported", path.c_str());
                return false;
            }
            else if (token == "DOMAIN_MIN")
            {
                iss >> domainMin[0] >> domainMin[1] >> domainMin[2];
            }
            else if (token == "DOMAIN_MAX")
            {
                iss >> domainMax[0] >> domainMax[1] >> domainMax[2];
            }
            else
            {
                float r = static_cast<float>(std::atof(token.c_str()));
                float g = 0.f, b = 0.f;
                iss >> g >> b;
                entries.push_back(r);
                entries.push_back(g);
                entries.push_back(b);
            }
        }

        if (size <= 1)
        {
            LutHdrTraceGpu(L"LoadTextCube: %s missing/invalid LUT_3D_SIZE", path.c_str());
            return false;
        }

        const size_t totalEntries = static_cast<size_t>(size) * size * size;
        if (entries.size() != totalEntries * 3)
        {
            LutHdrTraceGpu(L"LoadTextCube: %s entry count mismatch (got %zu, expected %zu for size=%d)",
                path.c_str(), entries.size() / 3, totalEntries, size);
            return false;
        }

        const bool nonDefaultDomain =
            domainMin[0] != 0.f || domainMin[1] != 0.f || domainMin[2] != 0.f ||
            domainMax[0] != 1.f || domainMax[1] != 1.f || domainMax[2] != 1.f;
        if (nonDefaultDomain)
        {
            LutHdrTraceGpu(L"LoadTextCube: %s has non-default DOMAIN_MIN/MAX (min=%.4f,%.4f,%.4f max=%.4f,%.4f,%.4f) "
                           L"- the GPU path samples this LUT on a raw [0,1] input with NO domain rescale, "
                           L"so this file will look wrong unless re-exported with the default 0-1 domain.",
                path.c_str(), domainMin[0], domainMin[1], domainMin[2], domainMax[0], domainMax[1], domainMax[2]);
        }

        outSize = size;
        outData.resize(totalEntries * 4);
        for (size_t i = 0; i < totalEntries; ++i)
        {
            outData[i * 4 + 0] = entries[i * 3 + 0];
            outData[i * 4 + 1] = entries[i * 3 + 1];
            outData[i * 4 + 2] = entries[i * 3 + 2];
            outData[i * 4 + 3] = 1.0f;
        }
        return true;
    }
}

namespace
{
    // CPU-side parsed LUT cache, shared across ALL D3D11LutEngine instances for the
    // life of the process. DirectShow constructs a brand-new CLutHdrFilter (and
    // therefore a brand-new D3D11LutEngine) for every file you open, so the
    // per-instance m_cachedPath* check below never helps across separate file
    // opens - every open of any HLG-processed file was re-reading and re-parsing
    // the LUT from disk from scratch, even for the exact same file as last time.
    // This cache persists for as long as the host player process stays alive
    // (e.g. across an entire MPC-BE playlist), so only the FIRST time a given LUT
    // file is used pays the disk-read/parse cost. Keyed on path + file size +
    // last-write-time so replacing/editing the LUT file on disk is still picked up
    // instead of serving stale data forever.
    struct CachedLutCpuData
    {
        int size = 0;
        std::vector<float> data;
        uint64_t fileSize = 0;
        FILETIME lastWriteTime{};
    };

    std::mutex g_lutCacheMutex;
    std::map<std::wstring, CachedLutCpuData> g_lutCpuCache;

    bool StatFile(const std::wstring& path, uint64_t& outSize, FILETIME& outWriteTime)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            return false;
        outSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        outWriteTime = fad.ftLastWriteTime;
        return true;
    }

    bool SameFileTime(const FILETIME& a, const FILETIME& b)
    {
        return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
    }

    uint32_t HashWString(const std::wstring& s)
    {
        uint32_t hash = 2166136261u;
        for (wchar_t c : s)
        {
            hash ^= static_cast<uint32_t>(c);
            hash *= 16777619u;
        }
        return hash;
    }

    std::wstring HashToHex(uint32_t h)
    {
        wchar_t buf[9];
        for (int i = 7; i >= 0; --i) { buf[i] = L"0123456789abcdef"[h & 0xF]; h >>= 4; }
        buf[8] = 0;
        return buf;
    }

    // The in-memory g_lutCpuCache above only survives as long as the host process
    // does - and MPC-BE (at least as configured/tested here) spawns a brand-new
    // process per file open rather than reusing one, so that cache was getting
    // wiped out on every single file open, not just the truly first one. This disk
    // cache (one small file per distinct LUT path, in %TEMP%, keyed by a hash of
    // the path) survives across process restarts the same way the compute shader
    // bytecode cache already does, so only the very first time a given LUT file is
    // ever used on this machine pays the full parse cost.
    struct LutCacheFileHeader
    {
        uint32_t magic;         // 'LHLC'
        uint64_t fileSize;
        FILETIME lastWriteTime;
        int32_t  lutSize;
        uint32_t floatCount;
    };
    constexpr uint32_t kLutCacheMagic = 0x4C484C43; // "LHLC"

    std::wstring GetLutCacheFilePath(const std::wstring& lutPath)
    {
        wchar_t tempDir[MAX_PATH]{};
        DWORD len = GetTempPathW(MAX_PATH, tempDir);
        if (len == 0 || len > MAX_PATH) return L"";
        return std::wstring(tempDir) + L"LutHdrFilter_lut_" + HashToHex(HashWString(lutPath)) + L".cache";
    }

    bool TryLoadLutDiskCache(const std::wstring& lutPath, uint64_t curFileSize, const FILETIME& curWriteTime,
                              int& outSize, std::vector<float>& outData)
    {
        const std::wstring cachePath = GetLutCacheFilePath(lutPath);
        if (cachePath.empty()) return false;

        std::ifstream file(cachePath, std::ios::binary);
        if (!file.is_open()) return false;

        LutCacheFileHeader header{};
        if (!file.read(reinterpret_cast<char*>(&header), sizeof(header))) return false;
        if (header.magic != kLutCacheMagic) return false;
        if (header.fileSize != curFileSize || !SameFileTime(header.lastWriteTime, curWriteTime)) return false;
        if (header.floatCount == 0 || header.floatCount > 256u * 256u * 256u * 4u) return false;

        outSize = header.lutSize;
        outData.resize(header.floatCount);
        if (!file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(header.floatCount) * sizeof(float)))
            return false;

        return true;
    }

    void SaveLutDiskCache(const std::wstring& lutPath, uint64_t fileSize, const FILETIME& writeTime,
                           int lutSize, const std::vector<float>& data)
    {
        const std::wstring cachePath = GetLutCacheFilePath(lutPath);
        if (cachePath.empty() || data.empty()) return;

        std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return; // best-effort; a failed write just means no disk cache next time

        LutCacheFileHeader header{ kLutCacheMagic, fileSize, writeTime, lutSize, static_cast<uint32_t>(data.size()) };
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()) * sizeof(float));
    }
}

bool D3D11LutEngine::LoadLutDirect(SourceColorSpace space, const std::wstring& path)
{
    if (!m_initialized || path.empty()) return false;

    // Fast Cache: Instant return if already in VRAM (helps re-opening a file within
    // the SAME filter instance, e.g. seeking; does NOT help across a fresh file
    // open, since that's a fresh D3D11LutEngine - see g_lutCpuCache above for that).
    if (space == SourceColorSpace::BT2020 && m_cachedPath2020 == path && m_srvLut2020) return true;
    if (space == SourceColorSpace::BT709  && m_cachedPath709  == path && m_srvLut709)  return true;
    if (space == SourceColorSpace::DCI_P3 && m_cachedPathDCIP3 == path && m_srvLutDCIP3) return true;

    uint64_t curFileSize = 0;
    FILETIME curWriteTime{};
    const bool haveStat = StatFile(path, curFileSize, curWriteTime);

    int size = 0;
    std::vector<float> lutData;
    bool loaded = false;

    {
        std::lock_guard<std::mutex> lock(g_lutCacheMutex);
        auto it = g_lutCpuCache.find(path);
        if (it != g_lutCpuCache.end() && haveStat &&
            it->second.fileSize == curFileSize && SameFileTime(it->second.lastWriteTime, curWriteTime))
        {
            size = it->second.size;
            lutData = it->second.data; // copy out under the lock, then release it
            loaded = true;
        }
    }

    if (loaded)
    {
        LutHdrTraceGpu(L"LoadLutDirect: process-wide CPU cache HIT for %s (size=%d, %zu floats) - skipped disk read/parse",
            path.c_str(), size, lutData.size());
    }
    else if (haveStat && TryLoadLutDiskCache(path, curFileSize, curWriteTime, size, lutData))
    {
        loaded = true;
        LutHdrTraceGpu(L"LoadLutDirect: on-disk parse cache HIT for %s (size=%d, %zu floats) - skipped re-parse (new process, previously-cached file)",
            path.c_str(), size, lutData.size());

        std::lock_guard<std::mutex> lock(g_lutCacheMutex);
        CachedLutCpuData& entry = g_lutCpuCache[path];
        entry.size = size;
        entry.data = lutData;
        entry.fileSize = curFileSize;
        entry.lastWriteTime = curWriteTime;
    }
    else
    {
        // Sniff the signature to decide binary .3dlut vs text .cube. A text .cube's
        // first 4 bytes are always ASCII (a comment, "TITLE", or "LUT_3D_SIZE" etc.),
        // never the "3DLT"/"3DL2" magic, so this dispatch is unambiguous.
        char sig[4]{};
        bool haveSig = false;
        {
            std::ifstream sniff(path, std::ios::binary);
            if (!sniff.is_open())
            {
                LutHdrTraceGpu(L"LoadLutDirect: could not open %s", path.c_str());
                return false;
            }
            haveSig = static_cast<bool>(sniff.read(sig, 4));
        }

        if (haveSig && (strncmp(sig, "3DLT", 4) == 0 || strncmp(sig, "3DL2", 4) == 0))
            loaded = LoadBinary3dlut(path, size, lutData);
        else
            loaded = LoadTextCube(path, size, lutData);

        if (!loaded)
        {
            LutHdrTraceGpu(L"LoadLutDirect: failed to load %s as either binary .3dlut or text .cube", path.c_str());
            return false;
        }

        if (haveStat)
        {
            std::lock_guard<std::mutex> lock(g_lutCacheMutex);
            CachedLutCpuData& entry = g_lutCpuCache[path];
            entry.size = size;
            entry.data = lutData;
            entry.fileSize = curFileSize;
            entry.lastWriteTime = curWriteTime;

            SaveLutDiskCache(path, curFileSize, curWriteTime, size, lutData);
        }
    }

    const size_t totalEntries = static_cast<size_t>(size) * size * size;

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width = size;
    desc.Height = size;
    desc.Depth = size;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subData{};
    subData.pSysMem = lutData.data();
    subData.SysMemPitch = size * sizeof(float) * 4;
    subData.SysMemSlicePitch = static_cast<size_t>(size) * size * sizeof(float) * 4;

    ComPtr<ID3D11Texture3D> pTex;
    HRESULT hr = m_device->CreateTexture3D(&desc, &subData, &pTex);
    if (FAILED(hr)) return false;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = m_device->CreateShaderResourceView(pTex.Get(), nullptr, &srv);
    if (FAILED(hr)) return false;

    switch (space)
    {
    case SourceColorSpace::BT2020:
        m_srvLut2020 = srv;
        m_cachedPath2020 = path;
        m_lutSize2020 = size;
        break;
    case SourceColorSpace::DCI_P3:
        m_srvLutDCIP3 = srv;
        m_cachedPathDCIP3 = path;
        m_lutSizeDCIP3 = size;
        break;
    case SourceColorSpace::BT709:
    default:
        m_srvLut709 = srv;
        m_cachedPath709 = path;
        m_lutSize709 = size;
        break;
    }

    LutHdrTraceGpu(L"LoadLutDirect: bound %s to GPU (size=%d, space=%d)",
        path.c_str(), size, static_cast<int>(space));

    return true;
}

bool D3D11LutEngine::EnsureTextures(int width, int height)
{
    if (m_texWidth == width && m_texHeight == height && m_texY_In) return true;

    m_texWidth = width;
    m_texHeight = height;

    m_texY_In.Reset();
    m_srvY_In.Reset();
    m_texUV_In.Reset();
    m_srvUV_In.Reset();
    m_texY_Out.Reset();
    m_uavY_Out.Reset();
    m_texUV_Out.Reset();
    m_uavUV_Out.Reset();
    m_texY_Staging.Reset();
    m_texUV_Staging.Reset();

    D3D11_TEXTURE2D_DESC descY{};
    descY.Width = width;
    descY.Height = height;
    descY.MipLevels = 1;
    descY.ArraySize = 1;
    descY.Format = DXGI_FORMAT_R16_UINT;
    descY.SampleDesc.Count = 1;
    descY.Usage = D3D11_USAGE_DEFAULT;
    descY.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&descY, nullptr, &m_texY_In);
    m_device->CreateShaderResourceView(m_texY_In.Get(), nullptr, &m_srvY_In);

    D3D11_TEXTURE2D_DESC descUV = descY;
    descUV.Width = width / 2;
    descUV.Height = height / 2;
    descUV.Format = DXGI_FORMAT_R16G16_UINT;
    m_device->CreateTexture2D(&descUV, nullptr, &m_texUV_In);
    m_device->CreateShaderResourceView(m_texUV_In.Get(), nullptr, &m_srvUV_In);

    descY.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    m_device->CreateTexture2D(&descY, nullptr, &m_texY_Out);
    m_device->CreateUnorderedAccessView(m_texY_Out.Get(), nullptr, &m_uavY_Out);

    descUV.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    m_device->CreateTexture2D(&descUV, nullptr, &m_texUV_Out);
    m_device->CreateUnorderedAccessView(m_texUV_Out.Get(), nullptr, &m_uavUV_Out);

    descY.BindFlags = 0;
    descY.Usage = D3D11_USAGE_STAGING;
    descY.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_device->CreateTexture2D(&descY, nullptr, &m_texY_Staging);

    descUV.BindFlags = 0;
    descUV.Usage = D3D11_USAGE_STAGING;
    descUV.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_device->CreateTexture2D(&descUV, nullptr, &m_texUV_Staging);

    return true;
}

bool D3D11LutEngine::ProcessFrameP010(
    const uint8_t* src,
    uint8_t* dst,
    int width,
    int height,
    int srcStrideBytes,
    int srcCodedHeight,
    int dstStrideBytes,
    int dstCodedHeight,
    const DetectedColorInfo& colorInfo)
{
    if (!m_initialized) return false;
    EnsureTextures(width, height);

    const int uvHeight = (height + 1) / 2;
    const uint8_t* srcY = src;
    // srcCodedHeight (not `height`) is where this specific source surface's UV plane
    // actually starts — they only coincide when the decoder didn't pad coded height.
    const uint8_t* srcUV = src + static_cast<size_t>(srcStrideBytes) * srcCodedHeight;

    m_context->UpdateSubresource(m_texY_In.Get(), 0, nullptr, srcY, srcStrideBytes, 0);
    m_context->UpdateSubresource(m_texUV_In.Get(), 0, nullptr, srcUV, srcStrideBytes, 0);

    ID3D11ShaderResourceView* activeLut = m_srvDefaultLut.Get();
    float activeLutSize = 2.0f;

    if (colorInfo.space == SourceColorSpace::BT2020 && m_srvLut2020) {
        activeLut = m_srvLut2020.Get();
        activeLutSize = float(m_lutSize2020);
    } else if (colorInfo.space == SourceColorSpace::DCI_P3 && m_srvLutDCIP3) {
        activeLut = m_srvLutDCIP3.Get();
        activeLutSize = float(m_lutSizeDCIP3);
    } else if (m_srvLut709) {
        activeLut = m_srvLut709.Get();
        activeLutSize = float(m_lutSize709);
    }

    YuvMatrixCoeffs inCoeffs = GetMatrixCoeffs(colorInfo.matrix);
    YuvMatrixCoeffs outCoeffs = GetMatrixCoeffs(SourceMatrix::BT709);

    D3D11_MAPPED_SUBRESOURCE mapCB;
    m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapCB);
    ShaderConstants* cb = static_cast<ShaderConstants*>(mapCB.pData);
    cb->in_kr = inCoeffs.kr;
    cb->in_kb = inCoeffs.kb;
    cb->out_kr = outCoeffs.kr;
    cb->out_kb = outCoeffs.kb;
    cb->width = width;
    cb->height = height;
    cb->is_limited = IsLimitedRange(colorInfo) ? 1 : 0;
    cb->lut_size = activeLutSize;
    cb->apply_ootf = m_applyHlgOotf ? 1 : 0;
    m_context->Unmap(m_constantBuffer.Get(), 0);

    m_context->CSSetShader(m_computeShader.Get(), nullptr, 0);
    m_context->CSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

    ID3D11ShaderResourceView* srvs[] = { m_srvY_In.Get(), m_srvUV_In.Get(), activeLut };
    m_context->CSSetShaderResources(0, 3, srvs);

    ID3D11SamplerState* samplers[] = { m_sampler.Get() };
    m_context->CSSetSamplers(0, 1, samplers);

    ID3D11UnorderedAccessView* uavs[] = { m_uavY_Out.Get(), m_uavUV_Out.Get() };
    m_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

    UINT dispatchX = (width / 2 + 15) / 16;
    UINT dispatchY = (height / 2 + 15) / 16;
    m_context->Dispatch(dispatchX, dispatchY, 1);

    ID3D11UnorderedAccessView* nullUavs[] = { nullptr, nullptr };
    m_context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);

    uint8_t* dstY = dst;
    // dstCodedHeight: where the OUTPUT buffer's own UV plane needs to start. This is
    // whatever the downstream allocator (e.g. a renderer's D3D-surface-backed one)
    // actually gave us for `out`, which is not guaranteed to equal `height` either.
    uint8_t* dstUV = dst + static_cast<size_t>(dstStrideBytes) * dstCodedHeight;

    D3D11_MAPPED_SUBRESOURCE mapY, mapUV;

    // Fast-path block copy
    m_context->CopyResource(m_texY_Staging.Get(), m_texY_Out.Get());
    m_context->Map(m_texY_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapY);
    if (mapY.RowPitch == dstStrideBytes) {
        memcpy(dstY, mapY.pData, static_cast<size_t>(dstStrideBytes) * height);
    } else {
        for (int y = 0; y < height; ++y) {
            memcpy(dstY + static_cast<size_t>(y) * dstStrideBytes,
                   static_cast<const uint8_t*>(mapY.pData) + static_cast<size_t>(y) * mapY.RowPitch,
                   width * 2);
        }
    }
    m_context->Unmap(m_texY_Staging.Get(), 0);

    m_context->CopyResource(m_texUV_Staging.Get(), m_texUV_Out.Get());
    m_context->Map(m_texUV_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapUV);
    if (mapUV.RowPitch == dstStrideBytes) {
        memcpy(dstUV, mapUV.pData, static_cast<size_t>(dstStrideBytes) * uvHeight);
    } else {
        for (int y = 0; y < uvHeight; ++y) {
            memcpy(dstUV + static_cast<size_t>(y) * dstStrideBytes,
                   static_cast<const uint8_t*>(mapUV.pData) + static_cast<size_t>(y) * mapUV.RowPitch,
                   width * 2);
        }
    }
    m_context->Unmap(m_texUV_Staging.Get(), 0);

    return true;
}