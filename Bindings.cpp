// SOURCEPORT: persist/restore KeyMap (keyboard) and PadMap (gamepad).
// See Bindings.h. The hot path is unchanged — this module only serialises
// both structs to controls.cfg.

#include "Bindings.h"
#include "Gamepad.h"
#include "Hunt.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace {

struct Action { const char* name; int* vk; int* pad; };

// Single source of truth for action→field mapping, used by both the Options
// UI and the config file. Keyboard VK is stored under "Name"; the gamepad
// button is stored under "Name.pad". "-1" means unbound on the pad.
Action* Table() {
    static Action tab[] = {
        { "Forward",    &KeyMap.fkForward,  &PadMap.fkForward  },
        { "Backward",   &KeyMap.fkBackward, &PadMap.fkBackward },
        { "TurnUp",     &KeyMap.fkUp,       &PadMap.fkUp       },
        { "TurnDown",   &KeyMap.fkDown,     &PadMap.fkDown     },
        { "TurnLeft",   &KeyMap.fkLeft,     &PadMap.fkLeft     },
        { "TurnRight",  &KeyMap.fkRight,    &PadMap.fkRight    },
        { "Fire",       &KeyMap.fkFire,     &PadMap.fkFire     },
        { "GetWeapon",  &KeyMap.fkShow,     &PadMap.fkShow     },
        { "StepLeft",   &KeyMap.fkSLeft,    &PadMap.fkSLeft    },
        { "StepRight",  &KeyMap.fkSRight,   &PadMap.fkSRight   },
        { "Strafe",     &KeyMap.fkStrafe,   &PadMap.fkStrafe   },
        { "Jump",       &KeyMap.fkJump,     &PadMap.fkJump     },
        { "Run",        &KeyMap.fkRun,      &PadMap.fkRun      },
        { "Crouch",     &KeyMap.fkCrouch,   &PadMap.fkCrouch   },
        { "Call",       &KeyMap.fkCall,     &PadMap.fkCall     },
        { "ChangeCall", &KeyMap.fkCCall,    &PadMap.fkCCall    },
        { "Binoculars", &KeyMap.fkBinoc,    &PadMap.fkBinoc    },
        { nullptr, nullptr, nullptr }
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

// Strip a trailing ".pad" suffix if present; returns true if it was a pad
// line. Modifies the key buffer in place.
bool IsPadKey(char* key) {
    int n = (int)std::strlen(key);
    const int kSuf = 4;   // ".pad"
    if (n > kSuf && std::strcmp(key + n - kSuf, ".pad") == 0) {
        key[n - kSuf] = 0;
        return true;
    }
    return false;
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

        bool isPad = IsPadKey(key);
        int v = std::atoi(val);

        bool matched = false;
        for (Action* a = Table(); a->name; ++a) {
            if (std::strcmp(a->name, key) == 0) {
                if (isPad) {
                    // Pad: -1 (unbound), 0..SDL_CONTROLLER_BUTTON_MAX-1 (real
                    // buttons), or PAD_VBTN_FIRST..PAD_VBTN_LAST (virtual
                    // buttons for triggers / stick directions). Anything
                    // else is skipped so a stale config can't install a
                    // nonsense binding.
                    bool ok = (v == -1) ||
                              (v >= 0 && v < SDL_CONTROLLER_BUTTON_MAX) ||
                              (v >= PAD_VBTN_FIRST && v <= PAD_VBTN_LAST);
                    if (ok) { *a->pad = v; ++loaded; }
                    else    { ++skipped; }
                } else {
                    if (v > 0 && v < 256) {
                        *a->vk = v;
                        ++loaded;
                    } else ++skipped;
                }
                matched = true;
                break;
            }
        }
        if (!matched) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                "[Bindings] unknown action '%s%s' — ignored\n",
                key, isPad ? ".pad" : "");
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
    std::fprintf(f, "# Keyboard: action=<Win32 VK decimal>. Gamepad: action.pad=<SDL button index, -1 unbound>.\n");
    std::fprintf(f, "# Edit via the in-game Options > Controls panel.\n");
    for (Action* a = Table(); a->name; ++a)
        std::fprintf(f, "%s=%d\n",     a->name, *a->vk);
    for (Action* a = Table(); a->name; ++a)
        std::fprintf(f, "%s.pad=%d\n", a->name, *a->pad);
    std::fclose(f);
    Log("[Bindings] saved controls.cfg\n");
}

} // namespace Bindings
