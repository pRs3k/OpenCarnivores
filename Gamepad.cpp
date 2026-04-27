// SOURCEPORT: SDL_GameController support. See Gamepad.h for design.

#include "Gamepad.h"
#include "Hunt.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// Pulled in from Hunt2.cpp — single shared input plumbing.
extern int   g_sdlMouseDX;
extern int   g_sdlMouseDY;
extern void  ToggleBinocular();
extern void  ToggleMapMode();
extern void  ChangeCall();
extern void  ToggleRunMode();
extern void  HideWeapon();

// Global, rebindable via Options>Controls. Defaults set in InitPadMap.
TPadMap PadMap;

namespace {

SDL_GameController* g_pad = nullptr;
SDL_JoystickID      g_padInstance = -1;

// Deadzone for the analog sticks. SDL axis range is ±32767. 8000 ≈ 24% —
// tight enough that small thumbstick drift doesn't drive motion, loose enough
// that gentle pushes register. Triggers have a tiny deadzone so light pulls
// don't unintentionally fire.
constexpr int    STICK_DEADZONE   = 8000;
constexpr int    TRIGGER_DEADZONE = 3000;
constexpr int    AXIS_MAX         = 32767;

// Right-stick sensitivity. Converts normalised [-1..1] into pixel-equivalent
// counts per ms, then integrated over TimeDt in Sample() to match how the
// mouse path accumulates WM_INPUT deltas between frames. Tuned so a full
// push gives roughly a 180° turn in ~0.6 s at OptMsSens=10.
constexpr float  LOOK_SENS_X = 0.9f;  // px per ms at full tilt
constexpr float  LOOK_SENS_Y = 0.7f;

// Apply deadzone then renormalise so the usable range still reaches ±1.0 —
// without this, the response curve would have a dead plateau followed by a
// jump at the threshold.
float NormAxis(int raw, int deadzone) {
    int mag = std::abs(raw);
    if (mag < deadzone) return 0.f;
    float sign = (raw < 0) ? -1.f : 1.f;
    float t = (float)(mag - deadzone) / (float)(AXIS_MAX - deadzone);
    if (t > 1.f) t = 1.f;
    // Square the magnitude for finer low-end control — typical shooter feel.
    return sign * t * t;
}

void OpenFirstAvailable() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (!gc) continue;
        g_pad = gc;
        g_padInstance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[Gamepad] opened: %s\n",
                      SDL_GameControllerName(gc));
        PrintLog(msg);
        return;
    }
}

void SwitchToNextWeapon(int dir) {
    // Cycle forward/backward through weapon slots 0..5, pick the first with
    // ammo — mirrors the number-key path's "no weapon" gate for empty slots.
    if (Weapon.FTime) return;  // mid-animation — matches number-key guard

    int start = TargetWeapon;
    for (int step = 1; step <= 6; ++step) {
        int w = ((start + dir * step) % 6 + 6) % 6;
        if (ShotsLeft[w] > 0) {
            TargetWeapon = w;
            if (!Weapon.state) CurrentWeapon = TargetWeapon;
            HideWeapon();
            return;
        }
    }
    AddMessage((LPSTR)"No weapon");
}

// Poke KeyboardState[vk] held/released. Using the Win32-VK-keyed array keeps
// ProcessControls' existing rising-edge detection (e.g. kfJump one-shot)
// working without a second code path.
void SetKey(int vk, bool held) {
    if (vk <= 0 || vk >= 256) return;
    if (held) KeyboardState[vk] |= 128;
    else      KeyboardState[vk] &= ~128;
}

// Axis → virtual-button state table. A virtual button is "pressed" when its
// axis crosses the threshold in the mapped direction. Edge-detected against
// g_vstate so Sample() / PollPadAxisEdge only fire on transitions.
struct VAxis { int vbtn; int axis; int sign; int threshold; };
constexpr int STICK_VBTN_THRESHOLD = (int)(AXIS_MAX * 0.35f);
VAxis g_vaxes[] = {
    { PAD_VBTN_LT,       SDL_CONTROLLER_AXIS_TRIGGERLEFT,  +1, TRIGGER_DEADZONE     },
    { PAD_VBTN_RT,       SDL_CONTROLLER_AXIS_TRIGGERRIGHT, +1, TRIGGER_DEADZONE     },
    { PAD_VBTN_LS_UP,    SDL_CONTROLLER_AXIS_LEFTY,        -1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_LS_DOWN,  SDL_CONTROLLER_AXIS_LEFTY,        +1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_LS_LEFT,  SDL_CONTROLLER_AXIS_LEFTX,        -1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_LS_RIGHT, SDL_CONTROLLER_AXIS_LEFTX,        +1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_RS_UP,    SDL_CONTROLLER_AXIS_RIGHTY,       -1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_RS_DOWN,  SDL_CONTROLLER_AXIS_RIGHTY,       +1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_RS_LEFT,  SDL_CONTROLLER_AXIS_RIGHTX,       -1, STICK_VBTN_THRESHOLD },
    { PAD_VBTN_RS_RIGHT, SDL_CONTROLLER_AXIS_RIGHTX,       +1, STICK_VBTN_THRESHOLD },
};
constexpr int kNumVAxes = (int)(sizeof(g_vaxes)/sizeof(g_vaxes[0]));
bool g_vstate[kNumVAxes] = {};  // indexed parallel to g_vaxes

// Read axis and decide if the virtual button is currently "active".
bool VAxisActive(const VAxis& v) {
    int raw = SDL_GameControllerGetAxis(g_pad, (SDL_GameControllerAxis)v.axis);
    return (v.sign > 0) ? (raw >  v.threshold) : (raw < -v.threshold);
}

// Dispatch a rebindable-action button press/release. Some actions are
// edge-triggered in the KEYDOWN handler in Hunt2.cpp (fkBinoc, fkCCall) and
// don't poll KeyboardState — those get invoked directly. Everything else
// is a straight SetKey(vk, held).
void DispatchAction(int vk, bool held) {
    if (!held) {
        // Release is a simple key clear. Edge-triggered actions had no key
        // pressed to begin with, so clearing their VK is a no-op.
        SetKey(vk, false);
        return;
    }
    if (vk == KeyMap.fkBinoc) { ToggleBinocular(); return; }
    if (vk == KeyMap.fkCCall) { ChangeCall();      return; }
    SetKey(vk, true);
}

// Walk the PadMap action list; for every action bound to `btn`, run the
// dispatcher. Multiple actions can share a button, and an unbound action
// (vk == -1 in PadMap) is skipped.
void DispatchButton(int btn, bool held) {
    struct Slot { int* pad; int vk; };
    Slot slots[] = {
        { &PadMap.fkForward,  KeyMap.fkForward  },
        { &PadMap.fkBackward, KeyMap.fkBackward },
        { &PadMap.fkUp,       KeyMap.fkUp       },
        { &PadMap.fkDown,     KeyMap.fkDown     },
        { &PadMap.fkLeft,     KeyMap.fkLeft     },
        { &PadMap.fkRight,    KeyMap.fkRight    },
        { &PadMap.fkFire,     KeyMap.fkFire     },
        { &PadMap.fkShow,     KeyMap.fkShow     },
        { &PadMap.fkSLeft,    KeyMap.fkSLeft    },
        { &PadMap.fkSRight,   KeyMap.fkSRight   },
        { &PadMap.fkStrafe,   KeyMap.fkStrafe   },
        { &PadMap.fkJump,     KeyMap.fkJump     },
        { &PadMap.fkRun,      KeyMap.fkRun      },
        { &PadMap.fkCrouch,   KeyMap.fkCrouch   },
        { &PadMap.fkCall,     KeyMap.fkCall     },
        { &PadMap.fkCCall,    KeyMap.fkCCall    },
        { &PadMap.fkBinoc,    KeyMap.fkBinoc    },
    };
    for (auto& s : slots) if (*s.pad == btn) DispatchAction(s.vk, held);
}

// Clear any held-key bits we may have set so a disconnect doesn't leave the
// player stuck holding a direction.
void ReleaseAllActionKeys() {
    SetKey(KeyMap.fkForward,  false);
    SetKey(KeyMap.fkBackward, false);
    SetKey(KeyMap.fkSLeft,    false);
    SetKey(KeyMap.fkSRight,   false);
    SetKey(KeyMap.fkFire,     false);
    SetKey(KeyMap.fkUp,       false);
    SetKey(KeyMap.fkDown,     false);
    SetKey(KeyMap.fkLeft,     false);
    SetKey(KeyMap.fkRight,    false);
    SetKey(KeyMap.fkJump,     false);
    SetKey(KeyMap.fkRun,      false);
    SetKey(KeyMap.fkCrouch,   false);
    SetKey(KeyMap.fkCall,     false);
    SetKey(KeyMap.fkShow,     false);
    SetKey(KeyMap.fkStrafe,   false);
    for (int i = 0; i < kNumVAxes; ++i) g_vstate[i] = false;
}

} // anon

// ─── PadMap defaults and button-name table ──────────────────────────────────

void InitPadMap() {
    PadMap.fkForward  = PAD_VBTN_LS_UP;
    PadMap.fkBackward = PAD_VBTN_LS_DOWN;
    PadMap.fkUp       = SDL_CONTROLLER_BUTTON_DPAD_UP;
    PadMap.fkDown     = SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    PadMap.fkLeft     = SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    PadMap.fkRight    = SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    PadMap.fkFire     = PAD_VBTN_RT;
    PadMap.fkShow     = -1;
    PadMap.fkSLeft    = PAD_VBTN_LS_LEFT;
    PadMap.fkSRight   = PAD_VBTN_LS_RIGHT;
    PadMap.fkStrafe   = -1;
    PadMap.fkJump     = SDL_CONTROLLER_BUTTON_A;
    PadMap.fkRun      = SDL_CONTROLLER_BUTTON_LEFTSTICK;
    PadMap.fkCrouch   = SDL_CONTROLLER_BUTTON_B;
    PadMap.fkCall     = SDL_CONTROLLER_BUTTON_X;
    PadMap.fkCCall    = SDL_CONTROLLER_BUTTON_Y;
    PadMap.fkBinoc    = PAD_VBTN_LT;
}

const char* PadBtnName(int btn) {
    switch (btn) {
    case SDL_CONTROLLER_BUTTON_A:             return "A";
    case SDL_CONTROLLER_BUTTON_B:             return "B";
    case SDL_CONTROLLER_BUTTON_X:             return "X";
    case SDL_CONTROLLER_BUTTON_Y:             return "Y";
    case SDL_CONTROLLER_BUTTON_BACK:          return "Back";
    case SDL_CONTROLLER_BUTTON_GUIDE:         return "Guide";
    case SDL_CONTROLLER_BUTTON_START:         return "Start";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return "LStick";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return "RStick";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return "LB";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB";
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return "DPad Up";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return "DPad Dn";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return "DPad L";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return "DPad R";
    case PAD_VBTN_LT:                         return "LT";
    case PAD_VBTN_RT:                         return "RT";
    case PAD_VBTN_LS_UP:                      return "LS Up";
    case PAD_VBTN_LS_DOWN:                    return "LS Dn";
    case PAD_VBTN_LS_LEFT:                    return "LS L";
    case PAD_VBTN_LS_RIGHT:                   return "LS R";
    case PAD_VBTN_RS_UP:                      return "RS Up";
    case PAD_VBTN_RS_DOWN:                    return "RS Dn";
    case PAD_VBTN_RS_LEFT:                    return "RS L";
    case PAD_VBTN_RS_RIGHT:                   return "RS R";
    default: return "(none)";
    }
}

namespace Gamepad {

void Init() {
    // SDL_INIT_GAMECONTROLLER implies JOYSTICK and EVENTS.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "[Gamepad] SDL_InitSubSystem failed: %s\n", SDL_GetError());
        PrintLog(msg);
        return;
    }
    // Let SDL map DS4/DS5/Switch Pro to the Xbox-360-style layout.
    SDL_GameControllerEventState(SDL_ENABLE);
    // SOURCEPORT: Some SDL backends (DInput, occasionally XInput with no prior
    // message loop) don't finalise joystick enumeration until the first event
    // pump. Pump once here so SDL_NumJoysticks() actually sees attached pads;
    // without this, OpenFirstAvailable bails silently and we rely on a later
    // CONTROLLERDEVICEADDED event — which menus historically consumed without
    // routing to Gamepad::HandleEvent.
    SDL_PumpEvents();
    OpenFirstAvailable();
    if (!g_pad) PrintLog((char*)"[Gamepad] no controller detected (will attach on hot-plug)\n");
}

void Shutdown() {
    if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = nullptr; g_padInstance = -1; }
}

bool Connected() { return g_pad != nullptr; }

bool HandleEvent(const SDL_Event& ev) {
    switch (ev.type) {
    case SDL_CONTROLLERDEVICEADDED:
        if (!g_pad) OpenFirstAvailable();
        return true;
    case SDL_CONTROLLERDEVICEREMOVED:
        if (g_pad && ev.cdevice.which == g_padInstance) {
            PrintLog((char*)"[Gamepad] disconnected\n");
            SDL_GameControllerClose(g_pad);
            g_pad = nullptr;
            g_padInstance = -1;
            ReleaseAllActionKeys();
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN: {
        if (!g_pad || ev.cbutton.which != g_padInstance) return false;
        // System-level bindings that aren't part of KeyMap (and therefore not
        // user-rebindable through the Options UI). Weapon swap, map toggle,
        // and the pause/exit menu stay fixed so you can always find them.
        switch (ev.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  SwitchToNextWeapon(-1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: SwitchToNextWeapon(+1); break;
        case SDL_CONTROLLER_BUTTON_BACK:
            if (!TrophyMode) ToggleMapMode();
            break;
        case SDL_CONTROLLER_BUTTON_START:
            if (TrophyMode) { SaveTrophy(); ExitTime = 1; }
            else if (PAUSE) { PAUSE = FALSE; }
            else { EXITMODE = !EXITMODE; }
            break;
        default: break;
        }
        // In addition, dispatch any PadMap-bound rebindable action(s). Users
        // who rebind e.g. LB to "Jump" get both behaviours — the weapon-swap
        // stays, the Jump extra is added.
        DispatchButton(ev.cbutton.button, true);
        return true;
    }
    case SDL_CONTROLLERBUTTONUP: {
        if (!g_pad || ev.cbutton.which != g_padInstance) return false;
        DispatchButton(ev.cbutton.button, false);
        return true;
    }
    }
    return false;
}

void Sample(float dtMs) {
    if (!g_pad) return;

    // ── Virtual-button edge detection for triggers and stick directions ──
    // Each axis crossing fires a DispatchButton press/release, so trigger/
    // stick bindings route through the same PadMap lookup as face/dpad
    // buttons. Rebinding LT, RT, or any stick direction is therefore a
    // straight PadMap edit — no special-case plumbing.
    for (int i = 0; i < kNumVAxes; ++i) {
        bool active = VAxisActive(g_vaxes[i]);
        if (active != g_vstate[i]) {
            g_vstate[i] = active;
            DispatchButton(g_vaxes[i].vbtn, active);
        }
    }

    // ── Right stick → analog mouselook deltas (not rebindable) ────────────
    // Right-stick direction is also exposed as virtual buttons above, but
    // the analog path runs unconditionally so "look" always feels smooth
    // regardless of how the user rebound the pad.
    float rx = NormAxis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX), STICK_DEADZONE);
    float ry = NormAxis(SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY), STICK_DEADZONE);

    // SOURCEPORT: link pad look sensitivity to the existing mouse-sens slider.
    // The downstream `mouseDX * (OptMsSens+64)/600/192` formula barely varies
    // across the 1..20 range (~0.88×..1.14×), which isn't noticeable on a pad.
    // Scaling the accumulated delta by OptMsSens/10 gives the slider a real
    // linear say on pad turn speed (0.1× at the low end, 2× at the high end)
    // while keeping the default (OptMsSens=10) identical to before.
    float padScale = (OptMsSens > 0) ? (float)OptMsSens / 10.0f : 1.0f;
    float addX = rx * LOOK_SENS_X * dtMs * padScale;
    float addY = ry * LOOK_SENS_Y * dtMs * padScale;
    g_sdlMouseDX += (int)addX;
    g_sdlMouseDY += (int)addY;
}

} // namespace Gamepad

int PollPadAxisEdge() {
    if (!g_pad) return 0;
    // Separate state table from the hunt-loop g_vstate so capturing a binding
    // in the Options screen doesn't confuse the hunt-time edge detector.
    static bool s_rebindState[kNumVAxes] = {};
    int fired = 0;
    for (int i = 0; i < kNumVAxes; ++i) {
        bool active = VAxisActive(g_vaxes[i]);
        if (active && !s_rebindState[i] && fired == 0) fired = g_vaxes[i].vbtn;
        s_rebindState[i] = active;
    }
    return fired;
}
