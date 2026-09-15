cmake_minimum_required(VERSION 3.20)
project(liblut_hdr_plugin LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This plugin uses D3D11 and only builds on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    ColorSpaceVLC.h
)

target_include_directories(lut_hdr_plugin PRIVATE
    ${VLC_SDK_DIR}/include
    ${VLC_SDK_DIR}/include/vlc/plugins   # vlc_common.h etc. live here, not directly under include/
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

set_target_properties(lut_hdr_plugin PROPERTIES
    OUTPUT_NAME "lut_hdr_plugin"
    PREFIX "lib"
)
