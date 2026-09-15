#pragma once

#include <vector>
#include <string>
#include <cstdint>

struct RgbF
{
    float r, g, b;
};

class Lut3D
{
public:
    bool LoadFile(const std::wstring& path);
    bool LoadCube(const std::wstring& path);
    bool Load3DLUT(const std::wstring& path);

    bool IsLoaded() const { return _size > 0 && !_data.empty(); }
    int  Size() const { return _size; }

    RgbF SampleGrid(int r, int g, int b) const;
    RgbF GetEntry(size_t i) const { return _data[i]; }
    RgbF Apply(RgbF in) const;

private:
    int _size = 0;
    std::vector<RgbF> _data;
    float _domainMin[3] = { 0.f, 0.f, 0.f };
    float _domainMax[3] = { 1.f, 1.f, 1.f };
};