cmake_minimum_required(VERSION 3.20)
project(OpenCarnivores LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SOURCEPORT: Select renderer backend. Use -DRENDERER=opengl to build the new
# SDL2+OpenGL 3.3 backend. Default is the original Direct3D 6 backend.
set(RENDERER "opengl" CACHE STRING "Renderer backend: d3d or opengl")

# SOURCEPORT: Suppress MSVC deprecation warnings for legacy C functions
# (sprintf, strcpy, etc.) that permeate the original 1999 source code.
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# SOURCEPORT: Disable specific warnings that flood the build from 1999-era code.
if(MSVC)
    add_compile_options(/W3 /wd4996 /wd4244 /wd4267)
    add_link_options(/MAP)
endif()

# ─── Renderer-specific setup ────────────────────────────────────────────────

if(RENDERER STREQUAL "opengl")
    message(STATUS "Building with OpenGL 3.3 + SDL2 renderer")
    add_compile_definitions(_opengl)

    set(SDL2_DIR   "${CMAKE_SOURCE_DIR}/deps/SDL2-2.30.12")
    set(GLAD_DIR   "${CMAKE_SOURCE_DIR}/deps/glad")
    set(OPENAL_DIR "${CMAKE_SOURCE_DIR}/deps/openal-soft")
    set(STB_DIR    "${CMAKE_SOURCE_DIR}/deps/stb")
    set(LUA_DIR    "${CMAKE_SOURCE_DIR}/deps/lua")

    # Lua 5.4 — compiled as C; lua.c and luac.c are the CLI entry points and
    # are excluded (we embed, not launch, the interpreter).
    file(GLOB LUA_SOURCES "${LUA_DIR}/src/*.c")
    list(FILTER LUA_SOURCES EXCLUDE REGEX ".*/lua\\.c$")
    list(FILTER LUA_SOURCES EXCLUDE REGEX ".*/luac\\.c$")

    set(RENDERER_SOURCES
        renderer/RendererGL.cpp
        ${GLAD_DIR}/src/gl.c
        ${LUA_SOURCES}
    )

    set(RENDERER_INCLUDES
        ${SDL2_DIR}/include
        ${GLAD_DIR}/include
        ${OPENAL_DIR}/include
        ${STB_DIR}
        ${LUA_DIR}/src
        ${CMAKE_SOURCE_DIR}          # so renderer/ headers resolve from project root
    )

    set(RENDERER_LIBS
        ${SDL2_DIR}/lib/x64/SDL2.lib
        ${SDL2_DIR}/lib/x64/SDL2main.lib
        ${OPENAL_DIR}/libs/Win64/OpenAL32.lib
        opengl32
    )

    # SDL2 needs SDL_main.h to redirect WinMain→main on Windows.
    # The executable must NOT be a WIN32 subsystem app for SDL_main to work
    # (SDL2main.lib handles the subsystem entry correctly).
    set(EXE_SUBSYSTEM "")    # console subsystem — SDL2main provides WinMain

else()
    message(STATUS "Building with Direct3D 6 renderer (legacy)")
    add_compile_definitions(_d3d)

    # SOURCEPORT: Use our local compat/ directory for stub DirectX 6 headers.
    include_directories(${CMAKE_SOURCE_DIR}/compat)

    set(RENDERER_SOURCES
        compat/dxguids.cpp
    )
    set(RENDERER_INCLUDES "")
    set(RENDERER_LIBS "")
    set(EXE_SUBSYSTEM WIN32)
endif()

# ─── Common sources ──────────────────────────────────────────────────────────

set(SOURCES
    Hunt2.cpp
    Game.cpp
    Characters.cpp
    Interface.cpp
    Resources.cpp
    mathematics.cpp
    Audio_DLL.cpp
    Audio_OpenAL.cpp
    TextureOverrides.cpp
    ModelOverrides.cpp
    Materials.cpp
    CustomMaterials.cpp
    DataDefs.cpp
    VFS.cpp
    Gamepad.cpp
    Bindings.cpp
    Scripting.cpp
    HotReload.cpp
    renderd3d.cpp
    Menu.cpp
    ${RENDERER_SOURCES}
    # SOURCEPORT: Loading.cpp excluded — Interface.cpp provides loading UI
    # SOURCEPORT: Audio.cpp excluded — original DirectSound/x86-ASM backend, replaced by Audio_OpenAL.cpp
    # SOURCEPORT: Audio_SDL.cpp retained in tree but not compiled — superseded by Audio_OpenAL.cpp
    # SOURCEPORT: Render3DFX.cpp excluded — requires defunct 3Dfx Glide SDK
    # SOURCEPORT: RenderSoft.cpp/renderasm.cpp excluded — requires x86 inline ASM
)

# ─── Executable ──────────────────────────────────────────────────────────────

add_executable(OpenCarnivores ${EXE_SUBSYSTEM}
    ${SOURCES}
    resource.rc
)

target_include_directories(OpenCarnivores PRIVATE
    ${RENDERER_INCLUDES}
)

# Link against Windows system libraries
target_link_libraries(OpenCarnivores PRIVATE
    kernel32
    user32
    gdi32
    winmm
    ole32
    ${RENDERER_LIBS}
)

# ─── Post-build: copy SDL2.dll next to the executable ────────────────────────

if(RENDERER STREQUAL "opengl")
    add_custom_command(TARGET OpenCarnivores POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_DIR}/lib/x64/SDL2.dll"
            $<TARGET_FILE_DIR:OpenCarnivores>
        COMMENT "Copying SDL2.dll"
    )
    add_custom_command(TARGET OpenCarnivores POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENAL_DIR}/bin/Win64/soft_oal.dll"
            "$<TARGET_FILE_DIR:OpenCarnivores>/OpenAL32.dll"
        COMMENT "Copying OpenAL32.dll (soft_oal.dll)"
    )
endif()
