// XR.h — OpenXR runtime integration (incremental; see XR.cpp for roadmap).
#pragma once

namespace XR {
    // Try to load openxr_loader.dll (Meta / SteamVR / Monado / WMR all ship
    // this when their runtime is active), create an instance, query the HMD
    // system, and — if the GL context is already current — create a session
    // bound to our OpenGL renderer. Safe to call when no runtime or no HMD
    // is present; logs and falls back to flat-screen.
    bool Init();

    // Pump xrPollEvent and drive the session state machine (READY →
    // xrBeginSession, STOPPING → xrEndSession, EXITING → release).
    // Must be called every frame once the GL renderer is up; no-op when
    // VR is unavailable.
    void PollEvents();

    // Release session, instance, and loader. Idempotent.
    void Shutdown();

    // True if the loader + instance are live.
    bool Available();

    // True if a session exists and is currently in a state where the
    // runtime wants us to submit frames (SYNCHRONIZED / VISIBLE / FOCUSED).
    // The stereo-render hook (step 6) will gate on this.
    bool SessionRunning();
}
