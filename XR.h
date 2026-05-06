// XR.h — OpenXR runtime integration (incremental; see XR.cpp for roadmap).
#pragma once
#include <stdint.h>

// ── VR controller button codes ────────────────────────────────────────────────
// Stored in TVRMap fields the same way SDL button indices live in TPadMap.
// -1 means unbound.
enum {
    VR_BTN_NONE          = 0,
    VR_BTN_FIRST         = 1,
    VR_BTN_TRIGGER_R     = 1,   // right index trigger
    VR_BTN_TRIGGER_L     = 2,   // left  index trigger
    VR_BTN_GRIP_R        = 3,   // right grip / squeeze
    VR_BTN_GRIP_L        = 4,   // left  grip / squeeze
    VR_BTN_THUMBSTICK_R  = 5,   // right thumbstick click
    VR_BTN_THUMBSTICK_L  = 6,   // left  thumbstick click
    VR_BTN_A             = 7,   // A (right controller — Oculus / Index)
    VR_BTN_B             = 8,   // B (right controller)
    VR_BTN_X             = 9,   // X (left  controller)
    VR_BTN_Y             = 10,  // Y (left  controller)
    // Virtual stick-direction buttons (threshold-detected from axes, bindable)
    VR_BTN_LS_UP         = 11,  // left stick pushed up    (+Y in OpenXR)
    VR_BTN_LS_DOWN       = 12,  // left stick pushed down  (-Y)
    VR_BTN_LS_LEFT       = 13,  // left stick pushed left  (-X)
    VR_BTN_LS_RIGHT      = 14,  // left stick pushed right (+X)
    VR_BTN_RS_UP         = 15,  // right stick up
    VR_BTN_RS_DOWN       = 16,  // right stick down
    VR_BTN_RS_LEFT       = 17,  // right stick left
    VR_BTN_RS_RIGHT      = 18,  // right stick right
    VR_BTN_LAST          = 18,
};

// One VR button binding per game action — mirrors TPadMap / TKeyMap layout.
struct TVRMap {
    int fkForward, fkBackward;
    int fkUp, fkDown, fkLeft, fkRight;
    int fkFire, fkShow;
    int fkSLeft, fkSRight;
    int fkStrafe, fkJump, fkRun, fkCrouch;
    int fkCall, fkCCall, fkBinoc;
};

// Global VR button map — accessible like KeyMap and PadMap (defined in XR.cpp).
extern TVRMap VRMap;

namespace XR {

// Populate VRMap with Quest-Touch defaults.  Call before Bindings::Load().
void InitVRMap();

// Human-readable name for a VR button code, e.g. "Trig R", "B".
// Returns "(none)" for -1 / out-of-range.
const char* VRBtnName(int btn);

// Per-frame: read VR button states and poke KeyboardState[] so ProcessControls
// sees them like any other input.  Mirrors Gamepad::Sample().
// No-op when the action system is unavailable.
void SampleVR();

// Rebind-UI helper.  Returns the VR_BTN_* code of the first button that newly
// became pressed since the previous call, or 0 if none.  Separate state table
// from SampleVR so capturing a binding in the Options screen does not interfere
// with in-game input.
int PollVRBtnEdge();
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
    bool SessionRunning();

    // Per-eye recommended render resolution (valid after swapchains are created).
    uint32_t EyeWidth();
    uint32_t EyeHeight();

    // Number of swapchain images per eye (same for both eyes).
    uint32_t SwapchainLength();

    // OpenGL FBO name for a specific swapchain image on a given eye.
    // eye = 0 (left) or 1 (right); imageIndex in [0, SwapchainLength()).
    // Step 6's stereo hook will acquire the current image index then
    // call EyeFBO(eye, currentIndex) to bind the render target.
    unsigned int EyeFBO(int eye, uint32_t imageIndex);

    // ── Step 5: per-frame XR frame loop ──────────────────────────────────────

    // Call once per rendered frame (when SessionRunning()) before any rendering.
    // Drives xrWaitFrame (pacing), xrBeginFrame, and xrLocateViews.
    // Returns true if the runtime wants eye images this frame AND views are valid.
    // Must be paired with EndFrame() even when it returns false.
    bool BeginFrame();

    // Call once per rendered frame after all rendering is done.
    // Submits xrEndFrame. Step 5 sends an empty layer (no HMD content yet);
    // step 6 will attach the eye swapchain images to a projection layer.
    // No-op if BeginFrame() was not called or failed.
    void EndFrame();

    // True if xrLocateViews succeeded for the current frame.
    // Valid between BeginFrame() and EndFrame().
    bool ViewsValid();

    // True when ViewsValid() and the session is live.
    // Gates the stereo render loop in Hunt2.cpp.
    bool StereoActive();

    // ── Step 6: per-eye render ────────────────────────────────────────────────

    // Acquire the next swapchain image for 'eye' (0=left, 1=right).
    // Returns the GL FBO name that should be bound as the render target.
    // Returns 0 on failure. Must be followed by ReleaseEyeImage().
    unsigned int AcquireEyeImage(int eye);

    // Record the projection view for 'eye' and release the swapchain image
    // back to the compositor. Must be called after AcquireEyeImage().
    // When eye==1 (right), marks both projection views ready for EndFrame.
    // symmetricFov=true: submit ±max half-angles for both eyes (use for 2D
    // menu blits where both eyes receive identical content — ensures the
    // compositor maps the image the same way for each eye).
    void ReleaseEyeImage(int eye, bool symmetricFov = false);

    // Compute per-eye camera globals from the xrLocateViews data.
    // Call before rendering each eye and restore the saved values after.
    //   alpha/beta — CameraAlpha / CameraBeta (yaw/pitch from HMD orientation)
    //   dx/dy/dz   — IPD offset in game units; ADD to the saved CameraX/Y/Z
    //   cw/ch      — CameraW / CameraH (derived from per-eye FOV angles)
    //   fovk       — FOVK (horizontal tan half-angle for water frustum culling)
    // vcxFrac/vcyFrac — principal-point fractions [0,1] of eye image width/height.
    //   VideoCX = (int)(vcxFrac * WinW), VideoCY = (int)(vcyFrac * WinH).
    //   For a symmetric display both are 0.5; Quest 3 is ~0.62/0.38 horizontally.
    void GetEyeCameraSetup(int eye,
        float& alpha, float& beta,
        float& dx,    float& dy,    float& dz,
        float& cw,    float& ch,    float& fovk,
        float& vcxFrac, float& vcyFrac);

    // Raw eye pose from xrLocateViews. Valid while ViewsValid() is true.
    //   pos[3]    — position (x,y,z) in the stage/local reference space.
    //   orient[4] — orientation quaternion (x,y,z,w).
    // Returns false if ViewsValid() is false or eye is out of range.
    bool GetEyePose(int eye, float pos[3], float orient[4]);

    // Raw FOV half-angles (radians) from xrLocateViews. Valid while ViewsValid().
    // Conventions: left/down are negative; right/up are positive.
    // Returns false if ViewsValid() is false or eye is out of range.
    bool GetEyeFov(int eye, float& left, float& right, float& up, float& down);

    // The predicted display time (XrTime, nanoseconds) for the frame currently
    // in flight. Valid between BeginFrame() and EndFrame(). Step 6 passes this
    // to xrEndFrame's XrFrameEndInfo.
    int64_t PredictedDisplayTime();

    // ── Menu quad layer — world-locked 2D panel ───────────────────────────────

    // Dimensions of the menu swapchain texture (pixels).
    uint32_t MenuImageWidth();
    uint32_t MenuImageHeight();

    // Call once when transitioning into menu mode to re-anchor the quad
    // in front of wherever the player is looking at that moment.
    void ResetMenuQuadPose();

    // Acquire the menu swapchain image for rendering/blitting.
    // Returns the GL FBO name, or 0 on failure.
    // Must be followed by ReleaseMenuImage() before EndFrame().
    unsigned int AcquireMenuImage();

    // Release the menu swapchain image and mark the quad layer ready
    // for submission in the next EndFrame().
    void ReleaseMenuImage();

    // ── Controller menu cursor ────────────────────────────────────────────────
    // Ray-casts the controller aim pose against the menu quad and returns the
    // corresponding game-window position.  Call each frame from MenuBegin().
    //   hand         — 0 = left, 1 = right.
    //   screenX/Y    — game screen coordinate (pixels, Y=0 at top).
    //   pressed      — trigger is currently held.
    //   justPressed  — trigger went down this frame (rising edge).
    //   justReleased — trigger went up this frame (falling edge).
    // Returns false if the controller is not active or not pointing at the quad.
    bool GetControllerMenuCursor(int hand,
        float& screenX, float& screenY,
        bool& pressed, bool& justPressed, bool& justReleased);

    // True once the controller action set has been created, bindings suggested,
    // and the session attached.  False if SetupActions() was never called or failed.
    bool ActionsReady();

    // Head-gaze cursor: ray-casts the HMD centre-forward direction against the
    // menu quad.  Uses the view pose already obtained by BeginFrame() — works
    // even when the controller action system is unavailable.
    // Returns false if ViewsValid() is false or the ray misses the quad.
    bool GetHeadGazeCursor(float& screenX, float& screenY);

    // ── Fix 2: HMD orientation → player look direction ────────────────────────

    // Returns the HMD centre yaw (alpha) and pitch (beta) in radians,
    // derived from the left-eye orientation quaternion (both eyes share the
    // same orientation; they differ only in IPD translation).
    // Intended for use in ProcessPlayerMovement() to replace mouse/key look
    // when StereoActive() is true.
    // Returns false (leaves yaw/pitch unchanged) if ViewsValid() is false.
    bool GetHeadOrientation(float& yaw, float& pitch);
}
