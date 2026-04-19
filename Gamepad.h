#pragma once
// SOURCEPORT: SDL_GameController support for the hunt loop.
//
// Design: gamepad state is poked into the same plumbing the keyboard uses
// (`KeyboardState[VK]` + `g_sdlMouseDX/DY`) so ProcessControls keeps a single
// code path. Edge-triggered actions (jump, weapon swap, map, pause, call) are
// dispatched from HandleEvent via the same functions the SDL_KEYDOWN case
// calls; held actions (move, look, fire) are applied in Sample.
//
// First-controller-wins. Hot-plug supported: a newly connected pad takes over
// if none is open; disconnects release the handle.
//
// PadMap is the gamepad equivalent of KeyMap: one SDL_GameControllerButton
// per rebindable action. -1 = unbound. Sticks and triggers are NOT in PadMap
// — they drive analog paths directly (left-stick → movement, right-stick →
// look, RT → fire, LT → binocular toggle).

#include <SDL.h>

namespace Gamepad {

void Init();                        // Must run after SDL_Init(SDL_INIT_GAMECONTROLLER).
void Shutdown();

// Dispatch SDL events that concern the controller:
//   SDL_CONTROLLERDEVICEADDED / REMOVED — hot-plug handling
//   SDL_CONTROLLERBUTTONDOWN           — edge-triggered actions (jump, pause, …)
// Returns true if the event was consumed.
bool HandleEvent(const SDL_Event& ev);

// Per-frame sampler: reads stick/trigger state, applies deadzones, writes
// held-key bits to KeyboardState[] and feeds right-stick motion into
// g_sdlMouseDX/DY so mouselook sensitivity applies uniformly.
// `dtMs` is the frame time in milliseconds (use TimeDt).
void Sample(float dtMs);

bool Connected();

} // namespace Gamepad

// Virtual button indices for the analog inputs, so triggers and stick
// directions are bindable just like face/dpad buttons. Values start at 64 to
// stay above SDL_CONTROLLER_BUTTON_MAX (currently ~21) with room to grow.
enum {
    PAD_VBTN_FIRST    = 64,
    PAD_VBTN_LT       = 64,
    PAD_VBTN_RT       = 65,
    PAD_VBTN_LS_UP    = 66,
    PAD_VBTN_LS_DOWN  = 67,
    PAD_VBTN_LS_LEFT  = 68,
    PAD_VBTN_LS_RIGHT = 69,
    PAD_VBTN_RS_UP    = 70,
    PAD_VBTN_RS_DOWN  = 71,
    PAD_VBTN_RS_LEFT  = 72,
    PAD_VBTN_RS_RIGHT = 73,
    PAD_VBTN_LAST     = 73,
};

struct TPadMap {
    int fkForward, fkBackward;
    int fkUp, fkDown, fkLeft, fkRight;
    int fkFire, fkShow;
    int fkSLeft, fkSRight;
    int fkStrafe, fkJump, fkRun, fkCrouch;
    int fkCall, fkCCall, fkBinoc;
};

extern TPadMap PadMap;

// Populate PadMap with Xbox-layout defaults. Called once on startup before
// Bindings::Load gets the chance to overlay the user's saved assignments.
void InitPadMap();

// Short human-readable name for a button index (e.g. "A", "DPad Up",
// "LStick"). Returns "(none)" for -1 / out-of-range.
const char* PadBtnName(int btn);

// Rebind-UI helper. Polls triggers and sticks on the currently-open pad and
// returns the first virtual button whose axis crossed threshold since the
// previous call, or 0 if none. State is kept internally so successive calls
// require the axis to settle back below threshold before firing again.
int PollPadAxisEdge();
