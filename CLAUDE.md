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
- **Mod compatibility**: all asset loaders are additive; never remove retail format parsers (.CAR, .3DF, .RSC, .MAP, .TGA, .WAV). New formats slot beside originals, never replace.

## Core files and architecture
See [ARCHITECTURE.md](ARCHITECTURE.md) for core game files and domain-specific guides.

## Domain documentation
- [RENDERING.md](RENDERING.md) — Rendering backends, texture override registry, multi-backend roadmap.
- [AUDIO.md](AUDIO.md) — Audio backends, OpenAL 3D positional audio, reverb and HRTF.
- [VR.md](VR.md) — OpenXR pipeline, HMD components, comfort features, head-tracking.
- [ROADMAP.md](ROADMAP.md) — Gameplay/engine/infrastructure todos.

## Keep docs in sync
Update relevant domain files when you:
- Discover new architectural patterns or file responsibilities.
- Complete or change roadmap items.
- Change major implementation approaches or decisions.
