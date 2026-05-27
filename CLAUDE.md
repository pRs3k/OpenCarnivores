Source port of Carnivores 2 — clean, maintainable engine on modern systems with full asset compatibility.

## Claude constraints
- No explanations, code only.
- Output only changed blocks.
- Answer in one sentence.

## Coding rules
- C++17 standard.
- Prefer `std::string`, `std::vector`, `std::array` over raw C arrays.
- `#pragma once` for headers.
- Keep original file/function names (traceability).
- Comment behavioral changes: `// SOURCEPORT: <description>`
- Use shell variables and relative paths, never hardcoded absolute paths.
- **Build system**: Use CMake only; do not create Visual Studio project files (.sln, .vcxproj). These are hand-maintained and become stale; the CMake toolchain is the source of truth.
- **Mod compatibility**: all asset loaders are additive; never remove retail format parsers (.CAR, .3DF, .RSC, .MAP, .TGA, .WAV). New formats slot beside originals, never replace.

## Core files and architecture
See [ARCHITECTURE.md](ARCHITECTURE.md) for core game files and domain-specific guides.

## Domain documentation
- [BUILD_REQUIREMENTS.md](BUILD_REQUIREMENTS.md) — Build system, CMake configuration, runtime dependencies (OpenXR, SDL2, OpenAL).
- [RENDERING.md](RENDERING.md) — Rendering backends, texture override registry, shader packs, multi-backend roadmap.
- [SHADER_PACKS.md](SHADER_PACKS.md) — Modding guide for creating custom shader effects and materials.
- [SHADER_DEVELOPMENT_NOTES.md](SHADER_DEVELOPMENT_NOTES.md) — Development history, design decisions, and future roadmap for shader system.
- [AUDIO.md](AUDIO.md) — Audio backends, OpenAL 3D positional audio, reverb and HRTF.
- [VR.md](VR.md) — OpenXR pipeline, HMD components, comfort features, head-tracking.
- [ROADMAP.md](ROADMAP.md) — Gameplay/engine/infrastructure todos.

## Keep docs in sync
Update relevant domain files when you:
- Discover new architectural patterns or file responsibilities.
- Complete or change roadmap items.
- Change major implementation approaches or decisions.
