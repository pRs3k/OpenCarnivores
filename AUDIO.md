# Audio System

## Files
- `Audio.cpp` / `Audio_DLL.cpp` — DirectSound audio system (legacy).
- `Audio_SDL.cpp` — SDL2 software mixer (fallback).
- `Audio_OpenAL.cpp` — OpenAL Soft backend (current default).
- `eax.h` — Creative EAX audio extensions header.

## Current OpenAL implementation
- 16 one-shot channels + 2 ambient slots with crossfade.
- Native 3D positional audio via `alSource3f` + AL_POSITION + AL_LINEAR_DISTANCE_CLAMPED.
- Per-voice EFX AL_LOWPASS filter for terrain occlusion.

## Roadmap
- EFX reverb zones: wire per-area reverb presets using `alGenEffects` (ready-made presets in `efx-presets.h` e.g. `EFX_REVERB_PRESET_FOREST`).
- HRTF toggle: `ALC_HRTF_SOFT` enable/disable via options menu; critical for VR immersion.
- Occlusion/obstruction via raycast against terrain mesh (currently `Audio_UploadGeometry` is a no-op stub).
- Move ambient fade tick out of `SDL_Audio_SetCameraPos` into a dedicated `SDL_Audio_Update()` called every frame regardless of game state (current snap-to-target in menus is a workaround).
