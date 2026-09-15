// Lut3D.cpp
#include "Lut3D.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <ppl.h>

bool Lut3D::LoadFile(const std::wstring& path)
{
    if (Load3DLUT(path))
        return true;
    return LoadCube(path);
}

bool Lut3D::Load3DLUT(const std::wstring& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    char sig[4];
    if (!file.read(sig, 4))
        return false;

    if (strncmp(sig, "3DLT", 4) != 0 && strncmp(sig, "3DL2", 4) != 0)
        return false;

    file.seekg(0, std::ios::end);
    size_t fileSize = static_cast<size_t>(file.tellg());

    size_t offset = 16384;
    int size = 0;

    if (fileSize > offset) {
        size_t dataBytes = fileSize - offset;
        if (dataBytes == 256ULL * 256 * 256 * 6) size = 256;
        else if (dataBytes == 65ULL * 65 * 65 * 6) size = 65;
        else if (dataBytes == 33ULL * 33 * 33 * 6) size = 33;
        else if (dataBytes == 17ULL * 17 * 17 * 6) size = 17;
    }

    if (size == 0)
    {
        if (fileSize >= 100663296) {
            size = 256;
            offset = fileSize - (256ULL * 256 * 256 * 6);
        } else if (fileSize >= 1647750) {
            size = 65;
            offset = fileSize - (65ULL * 65 * 65 * 6);
        } else {
            return false;
        }
    }

    file.seekg(offset, std::ios::beg);
    const size_t totalEntries = static_cast<size_t>(size) * size * size;
    std::vector<uint16_t> rawBuffer(totalEntries * 3);
    if (!file.read(reinterpret_cast<char*>(rawBuffer.data()), totalEntries * 3 * sizeof(uint16_t)))
        return false;

    _data.resize(totalEntries);

    // FIX: Channel 0 is Blue, Channel 1 is Red, Channel 2 is Green
    concurrency::parallel_for(size_t(0), totalEntries, [&](size_t i)
    {
        uint16_t rawB = rawBuffer[i * 3 + 0];
        uint16_t rawR = rawBuffer[i * 3 + 1];
        uint16_t rawG = rawBuffer[i * 3 + 2];

        _data[i] = RgbF{
            float(rawR) / 65535.0f,
            float(rawG) / 65535.0f,
            float(rawB) / 65535.0f
        };
    });

    _size = size;
    _domainMin[0] = 0.0f; _domainMin[1] = 0.0f; _domainMin[2] = 0.0f;
    _domainMax[0] = 1.0f; _domainMax[1] = 1.0f; _domainMax[2] = 1.0f;
    return true;
}

bool Lut3D::LoadCube(const std::wstring& path)
{
    std::ifstream file(path);
    if (!file.is_open())
        return false;

    std::vector<RgbF> entries;
    int size = 0;
    float domainMin[3] = { 0.f, 0.f, 0.f };
    float domainMax[3] = { 1.f, 1.f, 1.f };

    std::string line;
    while (std::getline(file, line))
    {
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            continue;
        line = line.substr(start);

        if (line.empty() || line[0] == '#')
            continue;

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
            RgbF v{};
            v.r = static_cast<float>(std::atof(token.c_str()));
            iss >> v.g >> v.b;
            entries.push_back(v);
        }
    }

    if (size <= 1)
        return false;

    const size_t expected = static_cast<size_t>(size) * size * size;
    if (entries.size() != expected)
        return false;

    _size = size;
    _data = std::move(entries);
    _domainMin[0] = domainMin[0]; _domainMin[1] = domainMin[1]; _domainMin[2] = domainMin[2];
    _domainMax[0] = domainMax[0]; _domainMax[1] = domainMax[1]; _domainMax[2] = domainMax[2];
    return true;
}

RgbF Lut3D::SampleGrid(int r, int g, int b) const
{
    return _data[static_cast<size_t>(r) + static_cast<size_t>(g) * _size +
                 static_cast<size_t>(b) * _size * _size];
}

RgbF Lut3D::Apply(RgbF in) const
{
    if (!IsLoaded())
        return in;

    auto normalize = [](float v, float lo, float hi)
    {
        if (hi <= lo) return 0.f;
        return std::clamp((v - lo) / (hi - lo), 0.f, 1.f);
    };

    const float maxIdx = static_cast<float>(_size - 1);
    const float nr = normalize(in.r, _domainMin[0], _domainMax[0]) * maxIdx;
    const float ng = normalize(in.g, _domainMin[1], _domainMax[1]) * maxIdx;
    const float nb = normalize(in.b, _domainMin[2], _domainMax[2]) * maxIdx;

    const int maxI = _size - 1;
    const int r0 = std::clamp(static_cast<int>(nr), 0, maxI);
    const int g0 = std::clamp(static_cast<int>(ng), 0, maxI);
    const int b0 = std::clamp(static_cast<int>(nb), 0, maxI);

    const int r1 = std::min(r0 + 1, maxI);
    const int g1 = std::min(g0 + 1, maxI);
    const int b1 = std::min(b0 + 1, maxI);

    const float fr = nr - r0, fg = ng - g0, fb = nb - b0;

    const size_t strideG = _size;
    const size_t strideB = static_cast<size_t>(_size) * _size;
    const RgbF* ptr = _data.data();

    const RgbF c000 = ptr[r0 + g0 * strideG + b0 * strideB];
    const RgbF c100 = ptr[r1 + g0 * strideG + b0 * strideB];
    const RgbF c010 = ptr[r0 + g1 * strideG + b0 * strideB];
    const RgbF c110 = ptr[r1 + g1 * strideG + b0 * strideB];
    const RgbF c001 = ptr[r0 + g0 * strideG + b1 * strideB];
    const RgbF c101 = ptr[r1 + g0 * strideG + b1 * strideB];
    const RgbF c011 = ptr[r0 + g1 * strideG + b1 * strideB];
    const RgbF c111 = ptr[r1 + g1 * strideG + b1 * strideB];

    auto lerp = [](RgbF a, RgbF b, float t)
    {
        return RgbF{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t };
    };

    const RgbF c00 = lerp(c000, c100, fr);
    const RgbF c10 = lerp(c010, c110, fr);
    const RgbF c01 = lerp(c001, c101, fr);
    const RgbF c11 = lerp(c011, c111, fr);

    const RgbF c0 = lerp(c00, c10, fg);
    const RgbF c1 = lerp(c01, c11, fg);

    return lerp(c0, c1, fb);
}