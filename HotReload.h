#pragma once
// SOURCEPORT: lightweight file-change watcher for dev hot reload.
//
// Polls registered file paths' modification timestamps from the main loop
// (throttled to ~3× per second). When a file's mtime advances past what was
// last observed, the callback runs. No threads, no OS notification APIs —
// keeps the implementation trivial and cross-platform-safe.
//
// Current wiring:
//   * Texture/material overrides (PNG/TGA/DDS/_normal/_mr/_ao) — re-registered
//     and their GPU cache entries evicted so the next bind re-uploads.
//   * Fragment/vertex shaders (shaders/basic.frag, shaders/basic.vert) —
//     recompiled; on compile/link failure the previous program stays bound.
//   * HUNTDAT\_res.txt — reparsed via LoadResourcesScript(); weapon/character
//     tables pick up the new values on the next frame.
//
// Opt out by defining OC_NO_HOTRELOAD at build time.

#include <functional>

namespace HotReload {

// Register `path` for watching. `onChange` runs on the main thread the next
// time Tick() observes the file's mtime has advanced.
void Watch(const char* path, std::function<void()> onChange);

// Poll registered files. Cheap to call every frame — internally throttles.
void Tick();

// Clear all watches (call before shutdown to avoid dangling callbacks).
void Shutdown();

} // namespace HotReload
