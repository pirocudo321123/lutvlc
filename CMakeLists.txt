cmake_minimum_required(VERSION 3.20)
project(liblut_hdr_plugin LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This plugin uses D3D11 and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# VLC_SDK_DIR must point at a VLC SDK/dev package containing:
#   include/vlc_common.h, vlc_plugin.h, vlc_filter.h, vlc_picture.h, ...
#   lib/libvlccore.lib (import lib for vlccore.dll)
# This is NOT bundled with a normal VLC install - grab the matching
# "-dev" / "third party" SDK archive from videolan.org, or build VLC itself
# once and point here at its include/ and the generated libvlccore import lib.
if(NOT VLC_SDK_DIR)
    message(FATAL_ERROR "Set -DVLC_SDK_DIR=<path to VLC SDK with include/ and lib/>")
endif()

add_library(lut_hdr_plugin SHARED
    lut_hdr_vlc.cpp
    D3D11LutEngine.cpp
    D3D11LutEngine.h
    Lut3D.cpp
    Lut3D.h
    YuvOps.cpp
    YuvOps.h
    PixelOps.cpp
    PixelOps.h
    ColorSpace.h
)

target_include_directories(lut_hdr_plugin PRIVATE
    ${VLC_SDK_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_directories(lut_hdr_plugin PRIVATE ${VLC_SDK_DIR}/lib)

target_link_libraries(lut_hdr_plugin PRIVATE
    libvlccore
    d3d11
    d3dcompiler
)

target_compile_definitions(lut_hdr_plugin PRIVATE
    __PLUGIN__
    MODULE_STRING="lut_hdr"
    UNICODE
    _UNICODE
    NOMINMAX
)

# VLC scans a "plugins" directory for files matching lib*_plugin.{dll,so} - the
# lib*_plugin naming (already reflected in the target name above) is required,
# not cosmetic.
set_target_properties(lut_hdr_plugin PROPERTIES
    OUTPUT_NAME "lut_hdr_plugin"
    PREFIX "lib"
)
