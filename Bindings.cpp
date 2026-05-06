// SOURCEPORT: persist/restore KeyMap (keyboard) and PadMap (gamepad).
// See Bindings.h. The hot path is unchanged — this module only serialises
// both structs to controls.cfg.

#include "Bindings.h"
#include "Gamepad.h"
#include "XR.h"
#include "Hunt.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace {

struct Action { const char* name; int* vk; int* pad; int* vr; };

// Single source of truth for action→field mapping.
// "Name"     = keyboard VK
// "Name.pad" = gamepad button (-1 unbound)
// "Name.vr"  = VR controller button (-1 unbound)
Action* Table() {
    static Action tab[] = {
        { "Forward",    &KeyMap.fkForward,  &PadMap.fkForward,  &VRMap.fkForward  },
        { "Backward",   &KeyMap.fkBackward, &PadMap.fkBackward, &VRMap.fkBackward },
        { "TurnUp",     &KeyMap.fkUp,       &PadMap.fkUp,       &VRMap.fkUp       },
        { "TurnDown",   &KeyMap.fkDown,     &PadMap.fkDown,     &VRMap.fkDown     },
        { "TurnLeft",   &KeyMap.fkLeft,     &PadMap.fkLeft,     &VRMap.fkLeft     },
        { "TurnRight",  &KeyMap.fkRight,    &PadMap.fkRight,    &VRMap.fkRight    },
        { "Fire",       &KeyMap.fkFire,     &PadMap.fkFire,     &VRMap.fkFire     },
        { "GetWeapon",  &KeyMap.fkShow,     &PadMap.fkShow,     &VRMap.fkShow     },
        { "StepLeft",   &KeyMap.fkSLeft,    &PadMap.fkSLeft,    &VRMap.fkSLeft    },
        { "StepRight",  &KeyMap.fkSRight,   &PadMap.fkSRight,   &VRMap.fkSRight   },
        { "Strafe",     &KeyMap.fkStrafe,   &PadMap.fkStrafe,   &VRMap.fkStrafe   },
        { "Jump",       &KeyMap.fkJump,     &PadMap.fkJump,     &VRMap.fkJump     },
        { "Run",        &KeyMap.fkRun,      &PadMap.fkRun,      &VRMap.fkRun      },
        { "Crouch",     &KeyMap.fkCrouch,   &PadMap.fkCrouch,   &VRMap.fkCrouch   },
        { "Call",       &KeyMap.fkCall,     &PadMap.fkCall,     &VRMap.fkCall     },
        { "ChangeCall", &KeyMap.fkCCall,    &PadMap.fkCCall,    &VRMap.fkCCall    },
        { "Binoculars", &KeyMap.fkBinoc,    &PadMap.fkBinoc,    &VRMap.fkBinoc    },
        { nullptr, nullptr, nullptr, nullptr }
    };
    return tab;
}

void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

// Trim trailing whitespace/CR/LF in place.
void RTrim(char* s) {
    int n = (int)std::strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) s[--n] = 0;
}

// Detect and strip a trailing ".pad" or ".vr" suffix.
// Returns 1 for pad, 2 for vr, 0 for plain key.  Modifies key in place.
int GetKeySuffix(char* key) {
    int n = (int)std::strlen(key);
    if (n > 4 && std::strcmp(key + n - 4, ".pad") == 0) { key[n-4] = 0; return 1; }
    if (n > 3 && std::strcmp(key + n - 3, ".vr")  == 0) { key[n-3] = 0; return 2; }
    return 0;
}

} // anon

namespace Bindings {

void ResetToDefaults() {
    // Keyboard: WASD + arrow-keys baseline (matches the retail Carnivores 2
    // defaults, with A/D moved to dedicated strafe as in modern shooters).
    KeyMap.fkForward  = 'W';
    KeyMap.fkBackward = 'S';
    KeyMap.fkSLeft    = 'A';
    KeyMap.fkSRight   = 'D';
    KeyMap.fkLeft     = VK_LEFT;
    KeyMap.fkRight    = VK_RIGHT;
    KeyMap.fkUp       = VK_UP;
    KeyMap.fkDown     = VK_DOWN;
    KeyMap.fkFire     = VK_LBUTTON;
    KeyMap.fkShow     = VK_RBUTTON;
    KeyMap.fkStrafe   = VK_CONTROL;
    KeyMap.fkJump     = VK_SPACE;
    KeyMap.fkRun      = VK_SHIFT;
    KeyMap.fkCrouch   = 'X';
    KeyMap.fkCall     = VK_MENU;
    KeyMap.fkCCall    = 'C';
    KeyMap.fkBinoc    = 'B';
    // Gamepad: Xbox layout; Gamepad.cpp owns the button map.
    InitPadMap();
    // VR: Quest Touch Plus layout; XR.cpp owns the button map.
    XR::InitVRMap();
}

void Load() {
    std::FILE* f = std::fopen("controls.cfg", "r");
    if (!f) {
        Log("[Bindings] controls.cfg not found — using defaults\n");
        return;
    }
    char line[256];
    int loaded = 0, skipped = 0;
    while (std::fgets(line, sizeof(line), f)) {
        RTrim(line);
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == 0 || *p == '#') continue;

        char* eq = std::strchr(p, '=');
        if (!eq) { ++skipped; continue; }
        *eq = 0;
        char* key = p;
        char* val = eq + 1;
        int kn = (int)std::strlen(key);
        while (kn > 0 && (key[kn-1] == ' ' || key[kn-1] == '\t')) key[--kn] = 0;
        while (*val == ' ' || *val == '\t') ++val;

        int suffix = GetKeySuffix(key);  // 0=key, 1=pad, 2=vr
        int v = std::atoi(val);

        bool matched = false;
        for (Action* a = Table(); a->name; ++a) {
            if (std::strcmp(a->name, key) == 0) {
                if (suffix == 1) {
                    bool ok = (v == -1) ||
                              (v >= 0 && v < SDL_CONTROLLER_BUTTON_MAX) ||
                              (v >= PAD_VBTN_FIRST && v <= PAD_VBTN_LAST);
                    if (ok) { *a->pad = v; ++loaded; }
                    else    { ++skipped; }
                } else if (suffix == 2) {
                    bool ok = (v == -1) ||
                              (v >= VR_BTN_FIRST && v <= VR_BTN_LAST);
                    if (ok) { *a->vr = v; ++loaded; }
                    else    { ++skipped; }
                } else {
                    if (v > 0 && v < 256) { *a->vk = v; ++loaded; }
                    else ++skipped;
                }
                matched = true;
                break;
            }
        }
        if (!matched) {
            static const char* kSuf[] = { "", ".pad", ".vr" };
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "[Bindings] unknown action '%s%s' — ignored\n",
                key, kSuf[suffix < 3 ? suffix : 0]);
            Log(msg);
            ++skipped;
        }
    }
    std::fclose(f);
    char msg[128];
    std::snprintf(msg, sizeof(msg),
        "[Bindings] loaded %d binding(s), skipped %d\n", loaded, skipped);
    Log(msg);
}

void Save() {
    std::FILE* f = std::fopen("controls.cfg", "w");
    if (!f) {
        Log("[Bindings] failed to write controls.cfg\n");
        return;
    }
    std::fprintf(f, "# OpenCarnivores input bindings.\n");
    std::fprintf(f, "# action=<VK>  action.pad=<SDL btn, -1 unbound>  action.vr=<VR_BTN_*, -1 unbound>\n");
    std::fprintf(f, "# Edit via the in-game Options > Controls panel.\n");
    for (Action* a = Table(); a->name; ++a)
        std::fprintf(f, "%s=%d\n",     a->name, *a->vk);
    for (Action* a = Table(); a->name; ++a)
        std::fprintf(f, "%s.pad=%d\n", a->name, *a->pad);
    for (Action* a = Table(); a->name; ++a)
        std::fprintf(f, "%s.vr=%d\n",  a->name, *a->vr);
    std::fclose(f);
    Log("[Bindings] saved controls.cfg\n");
}

} // namespace Bindings
