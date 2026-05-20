# Audio System

## Files
- `Audio_SDL.cpp` — SDL2 software mixer (fallback).
- `Audio_OpenAL.cpp` — OpenAL Soft backend (current default).

## Current OpenAL implementation
- 16 one-shot channels + 2 ambient slots with crossfade.
- Native 3D positional audio via `alSource3f` + AL_POSITION + AL_LINEAR_DISTANCE_CLAMPED.
- Per-voice EFX AL_LOWPASS filter for terrain occlusion.
- **5.1 surround sound**: Automatic detection and support if available on the device.

## 5.1 Surround Sound

OpenCarnivores supports 5.1 surround output (Front Left/Right, Center, LFE, Side Left/Right) when available on the system.

**Detection**: Automatic at startup — queries OpenAL device for channel capacity.

**Audio Assets**: Existing mono sources are automatically virtualized across 5.1 speaker layout:
- Mono buffers (22050 Hz 16-bit PCM) fed to individual spatial sources
- OpenAL's 3D positioning distributes sounds to surround speakers based on:
  - Source 3D position relative to listener
  - Listener orientation (forward/up vectors)
  - Distance attenuation model
  - Terrain occlusion (muffles rear sounds when blocked)
  - Environmental reverb zones

**Graceful Fallback**: If 5.1 unavailable, falls back to stereo with identical behavior.

**Requirements**: OpenAL Soft 1.19.1+ (ALC_CHANNELS and AL_EXT_MCFORMATS support) or hardware with native surround.

## Roadmap
- EFX reverb zones: wire per-area reverb presets using `alGenEffects` (ready-made presets in `efx-presets.h` e.g. `EFX_REVERB_PRESET_FOREST`).
- HRTF toggle: `ALC_HRTF_SOFT` enable/disable via options menu; critical for VR immersion.
- Occlusion/obstruction via raycast against terrain mesh (currently `Audio_UploadGeometry` is a no-op stub).
- Move ambient fade tick out of `SDL_Audio_SetCameraPos` into a dedicated `SDL_Audio_Update()` called every frame regardless of game state (current snap-to-target in menus is a workaround).
