# Build Requirements

## Runtime Dependencies

### OpenXR Loader (openxr_loader.dll)
**Status:** Included in repository at `openxr_loader.dll`

The OpenXR loader is required for VR support via Meta Horizon Link. It is now included in the repo for end-user convenience — no manual setup required.

- **For Release Builds:** CMakeLists.txt automatically copies openxr_loader.dll to the build output
- **For Development:** The DLL is available in the repo root; build system finds and stages it
- **If Missing:** VR mode silently fails to launch (flatscreen still works). Rebuild from clean clone to restore.

## Windows SDK & Toolchain

- **Windows SDK:** 10.0.26100.0 or later
- **MSVC:** Visual Studio 2022 (v143 toolset)
- **CMake:** 3.20+
- **Git:** For clone/checkout

## SDL2 & OpenAL

Precompiled binaries for these dependencies are committed to `deps/`:
- `deps/SDL2-2.30.12/` — headers and libs (binary DLL copied at build time)
- `deps/openal-soft/` — headers and libs (binary DLL copied at build time)

Both are auto-detected and staged to build output by CMakeLists.txt.

## Build Procedure

```bash
cd OpenCarnivores
mkdir -p build
cd build
cmake -G "Visual Studio 17 2022" -DRENDERER=opengl ..
cmake --build . --config Release
```

Output: `build/Release/OpenCarnivores.exe` + all runtime DLLs (SDL2.dll, OpenAL32.dll, openxr_loader.dll)

## Troubleshooting

### VR Not Launching
**Symptom:** Game runs in flatscreen but VR mode fails to start in Meta Horizon Link.

**Cause:** openxr_loader.dll missing from build output.

**Fix:** Rebuild from scratch:
```bash
rm -rf build/
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -DRENDERER=opengl ..
cmake --build . --config Release
```

The DLL will be auto-copied from the repo root to `build/Release/`.

### Missing Header Files
**Symptom:** Compile error like `fatal error C1083: Cannot open include file: 'stb_image.h'`

**Cause:** deps/ directory incomplete or corrupted.

**Fix:** Verify `deps/stb/stb_image.h` exists; if not, reclone the repo.

## Release Packaging

When creating releases:
1. Ensure `openxr_loader.dll` is in the root of the archive
2. Include all shader files: `shaders/` directory
3. Verify all DLLs are in the .zip alongside the .exe:
   - `OpenCarnivores.exe`
   - `SDL2.dll`
   - `OpenAL32.dll`
   - `openxr_loader.dll`

See [README.md](README.md) VR prerequisites section for end-user documentation.

## Future Enhancements

- **GitHub Artifact Repository:** In a future update, pre-built binaries could be hosted as GitHub releases, eliminating the need to commit the 2.3 MB openxr_loader.dll to the repo.
- **GitHub Actions:** Automated builds per commit would catch build failures early.
