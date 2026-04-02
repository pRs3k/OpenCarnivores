cmake_minimum_required(VERSION 3.20)
project(OpenCarnivores LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SOURCEPORT: Build with Direct3D 6 renderer path (_d3d).
# The software renderer (_soft) and Glide renderer (_3dfx) are excluded
# because _soft requires x86-only inline ASM and _3dfx requires the
# defunct 3Dfx Glide SDK.
add_compile_definitions(_d3d)

# SOURCEPORT: Suppress MSVC deprecation warnings for legacy C functions
# (sprintf, strcpy, etc.) that permeate the original 1999 source code.
# These will be replaced in a later phase.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# SOURCEPORT: Use our local compat/ directory for stub DirectX 6 headers.
# The original code needs ddraw.h and d3d.h which are not in the modern
# Windows SDK. These stubs provide just enough type definitions to compile.
include_directories(${CMAKE_SOURCE_DIR}/compat)

# SOURCEPORT: Disable specific warnings that flood the build from 1999-era code.
# C4996: deprecated functions (covered by _CRT_SECURE_NO_WARNINGS but belt-and-suspenders)
# C4244: conversion warnings (float/double/int narrowing everywhere in original)
# C4267: size_t to int (64-bit pointer width issues)
if(MSVC)
    add_compile_options(/W3 /wd4996 /wd4244 /wd4267)
    # SOURCEPORT: Generate map file for crash address resolution during development
    add_link_options(/MAP)
endif()

set(SOURCES
    Hunt2.cpp
    Game.cpp
    Characters.cpp
    Interface.cpp
    Resources.cpp
    mathematics.cpp
    Audio_DLL.cpp
    renderd3d.cpp
    compat/dxguids.cpp
    # SOURCEPORT: Excluded — not in original D3D build; Interface.cpp provides loading UI
    # Loading.cpp
    # SOURCEPORT: Excluded — compiled as separate DLL (a_soft.dll), not linked into main exe
    # Audio.cpp
    # SOURCEPORT: Excluded — requires 3Dfx Glide SDK (defunct hardware)
    # Render3DFX.cpp
    # SOURCEPORT: Excluded — requires _soft define and x86 inline ASM
    # RenderSoft.cpp
    # renderasm.cpp
)

add_executable(OpenCarnivores WIN32
    ${SOURCES}
    resource.rc
)

# Link against Windows system libraries the original code uses
# SOURCEPORT: dsound and dxguid are not linked — Audio_DLL.cpp loads audio
# DLLs at runtime, and DirectX GUIDs are defined in compat/dxguids.cpp.
# DirectDrawCreate is loaded at runtime via ddraw.dll in the original code,
# but renderd3d.cpp calls it directly, so we link ddraw.lib.
target_link_libraries(OpenCarnivores PRIVATE
    kernel32
    user32
    gdi32
    winmm
    ole32
)
