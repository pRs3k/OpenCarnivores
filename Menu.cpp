// SOURCEPORT: Menu system for OpenCarnivores — uses original Carnivores 2 artwork.
// Each menu screen is an 800x600 TGA pair (OFF/ON) plus a 400x300 RAW hit-test map.
// The hit-test map encodes button regions: each pixel byte is a button ID (0=background).
// On hover: pixels where map==hoveredId are taken from the ON image, rest from OFF image.
// This faithfully reproduces the original D3D menu visual style.

#include "Hunt.h"
#include <SDL.h>
#include "renderer/RendererGL.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>
#undef min
#undef max

extern RendererGL* g_glRenderer;

// ─── Types ───────────────────────────────────────────────────────────────────

struct MenuScreen {
    TPicture off  = {};   // normal TGA
    TPicture on   = {};   // highlighted TGA
    TPicture comp = {};   // composite output (CPU-blended each frame)
    std::vector<uint8_t> map; // 400x300 hit-test map
    int mapW = 400, mapH = 300;
    bool loaded = false;
};

// ─── Menu audio helpers ───────────────────────────────────────────────────────
// gLastHov: track hover ID so MENUMOV plays only on enter, not every frame.
// gMenuAmbActive: only call SetAmbient when ambient actually changes, to avoid
//   audible loop restarts when navigating between screens that share the same track.
static int    gLastHov      = -1;
static short* gMenuAmbActive = nullptr;

// Start MENUAMB looping if it isn't already the current ambient.
static void MenuStartAmb() {
    if (fxMenuAmb.lpData && fxMenuAmb.lpData != gMenuAmbActive) {
        gMenuAmbActive = fxMenuAmb.lpData;
        SetAmbient(fxMenuAmb.length, fxMenuAmb.lpData, 192);
    }
}

// ─── Mouse state (updated by PollMenuEvents) ─────────────────────────────────
// x/y are in GL drawable coordinates (= WinW × WinH space), not raw window pixels.

static struct {
    int x = 0, y = 0;   // scaled to WinW × WinH drawable space
    bool lClick  = false;
    bool rClick  = false;
    bool lHeld   = false;  // true while left button is held down (for drag)
    int  scancode = 0;
} gMI;

// Scale raw SDL logical mouse coords → GL drawable coords.
// SDL reports in logical window pixels; WinW/WinH are drawable pixels (may differ on HiDPI).
static void ScaleMouse(int rawX, int rawY) {
    int logW, logH;
    SDL_GetWindowSize(g_glRenderer->GetWindow(), &logW, &logH);
    if (logW <= 0) logW = WinW;
    if (logH <= 0) logH = WinH;
    gMI.x = rawX * WinW / logW;
    gMI.y = rawY * WinH / logH;
}

// ─── SDL event pump for menus ─────────────────────────────────────────────────

static bool PollMenuEvents(bool& appQuit) {
    gMI.lClick = false;
    gMI.rClick = false;
    gMI.scancode = 0;
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:    appQuit = true; return false;
        case SDL_MOUSEMOTION:
            ScaleMouse(ev.motion.x, ev.motion.y); break;
        case SDL_MOUSEBUTTONDOWN:
            ScaleMouse(ev.button.x, ev.button.y);
            if (ev.button.button == SDL_BUTTON_LEFT)  { gMI.lClick = true; gMI.lHeld = true; }
            if (ev.button.button == SDL_BUTTON_RIGHT) gMI.rClick = true;
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) gMI.lHeld = false;
            break;
        case SDL_KEYDOWN:
            gMI.scancode = ev.key.keysym.scancode; break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                int dw, dh;
                SDL_GL_GetDrawableSize(g_glRenderer->GetWindow(), &dw, &dh);
                if (dw > 0 && dh > 0) {
                    extern void SetVideoMode(int, int);
                    SetVideoMode(dw, dh);
                    glViewport(0, 0, dw, dh);
                }
            }
            break;
        }
    }
    return true;
}

// ─── Safe TGA loader (returns false instead of DoHalt on missing files) ───────

static bool SafeLoadTGA(TPicture& pic, const char* path) {
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hf);
    LoadPictureTGA(pic, (LPSTR)path);
    conv_pic(pic);   // RGB555 → RGB565 for GL
    return true;
}

static void FreePic(TPicture& pic) {
    if (pic.lpImage) { _HeapFree(Heap, 0, pic.lpImage); pic.lpImage = nullptr; }
    pic.W = pic.H = 0;
}

// Load a MenuScreen: OFF tga, ON tga (optional), RAW map (optional).
static bool LoadMenuScreen(MenuScreen& ms, const char* offPath,
                           const char* onPath = nullptr,
                           const char* mapPath = nullptr)
{
    if (ms.loaded) return true;

    if (!SafeLoadTGA(ms.off, offPath)) return false;

    // Allocate composite buffer same size as OFF image
    ms.comp.W = ms.off.W; ms.comp.H = ms.off.H;
    ms.comp.lpImage = (WORD*)_HeapAlloc(Heap, 0, ms.comp.W * ms.comp.H * 2);
    memcpy(ms.comp.lpImage, ms.off.lpImage, ms.comp.W * ms.comp.H * 2);

    if (onPath) SafeLoadTGA(ms.on, onPath);

    if (mapPath) {
        FILE* f = fopen(mapPath, "rb");
        if (f) {
            ms.map.resize(ms.mapW * ms.mapH, 0);
            fread(ms.map.data(), 1, ms.mapW * ms.mapH, f);
            fclose(f);
        }
    }

    ms.loaded = true;
    return true;
}

static void FreeMenuScreen(MenuScreen& ms) {
    FreePic(ms.off);
    FreePic(ms.on);
    FreePic(ms.comp);
    ms.map.clear();
    ms.loaded = false;
}

// ─── Core rendering ──────────────────────────────────────────────────────────

// CPU-blend OFF+ON based on map hotspot for the hovered button.
// alwaysOn: additional button IDs to show in ON state regardless of hover (for selected state).
// Returns the button ID the mouse is currently over (0 = none).
static int CompositeMenu(MenuScreen& ms, const std::vector<int>& alwaysOn = {}) {
    if (!ms.comp.lpImage || !ms.off.lpImage) return 0;

    // Map mouse position to 400x300 map coords (map is half the 800x600 image)
    int mx = (gMI.x * ms.mapW) / WinW;
    int my = (gMI.y * ms.mapH) / WinH;
    mx = std::max(0, std::min(mx, ms.mapW - 1));
    my = std::max(0, std::min(my, ms.mapH - 1));

    int hoverId = 0;
    if (!ms.map.empty())
        hoverId = (int)(uint8_t)ms.map[my * ms.mapW + mx];
    // '*' (42) appears in some maps as a generic "is a button" marker — treat it
    // as a sentinel that means "something is hovered but use ID from nearby pixel"
    if (hoverId == '*') hoverId = 0;

    // Blend: start from off, replace hovered-button and always-on pixels with on
    bool needBlend = ms.on.lpImage && (hoverId > 0 || !alwaysOn.empty());
    if (needBlend) {
        int scaleX = ms.off.W / ms.mapW;   // 2
        int scaleY = ms.off.H / ms.mapH;   // 2
        WORD* dst  = ms.comp.lpImage;
        WORD* offP = ms.off.lpImage;
        WORD* onP  = ms.on.lpImage;

        for (int iy = 0; iy < ms.mapH; iy++) {
            for (int ix = 0; ix < ms.mapW; ix++) {
                uint8_t id = ms.map.empty() ? 0 : (uint8_t)ms.map[iy * ms.mapW + ix];
                bool useOn = (id != 0) && (
                    (id == (uint8_t)hoverId) ||
                    std::find(alwaysOn.begin(), alwaysOn.end(), (int)id) != alwaysOn.end()
                );
                for (int dy = 0; dy < scaleY; dy++) {
                    int py = iy * scaleY + dy;
                    WORD* src = useOn ? (onP + py * ms.off.W + ix * scaleX)
                                      : (offP + py * ms.off.W + ix * scaleX);
                    WORD* d   = dst + py * ms.comp.W + ix * scaleX;
                    for (int dx = 0; dx < scaleX; dx++)
                        d[dx] = src[dx];
                }
            }
        }
    } else {
        // No hover and no always-on — copy off straight to comp
        memcpy(ms.comp.lpImage, ms.off.lpImage, ms.comp.W * ms.comp.H * 2);
    }

    // ── Menu sounds ──
    if (hoverId != gLastHov) {
        if (hoverId > 0 && fxMenuMov.lpData)
            AddVoicev(fxMenuMov.length, fxMenuMov.lpData, 160);
        gLastHov = hoverId;
    }
    if (gMI.lClick && hoverId > 0 && fxMenuGo.lpData)
        AddVoicev(fxMenuGo.length, fxMenuGo.lpData, 220);

    return hoverId;
}

// Draw a MenuScreen composite fullscreen.
static void DrawMenuScreen(MenuScreen& ms) {
    if (!ms.comp.lpImage) return;
    // Scale to WinW x WinH, no colorkey (solid background)
    g_glRenderer->DrawBitmap(0, 0, WinW, WinH,
                             ms.comp.W, ms.comp.lpImage, false, ms.comp.H);
}

// Overlay a sub-image (like an area/dino preview) on top of the current frame.
static void OverlayPic(TPicture& pic, int x, int y, int w, int h) {
    if (!pic.lpImage || pic.W <= 0 || pic.H <= 0) return;
    g_glRenderer->DrawBitmap(x, y, w, h, pic.W, pic.lpImage, false, pic.H);
}

// Draw simple text using the renderer's text system.
static void MT(const char* s, int x, int y, uint32_t col = 0x00FFFFFF) {
    if (s && s[0]) {
        g_glRenderer->DrawText(x+1, y+1, s, 0x00000000);
        g_glRenderer->DrawText(x,   y,   s, col);
    }
}

// Draw medium-weight text (fnt_Midd style: semibold, 16px at 600p) — used for NAME/SCORE bars.
static void MTMed(const char* s, int x, int y, uint32_t col = 0x00FFFFFF) {
    if (s && s[0]) {
        int sh = std::max(1, WinH / 720);  // 1px at ≤720p, 2px at 1440p, 3px at 2160p
        g_glRenderer->DrawTextMed(x+sh, y+sh, s, 0x00000000);
        g_glRenderer->DrawTextMed(x,    y,    s, col);
    }
}

// Read a text file up to maxBytes.
static std::string ReadTextFile(const char* path, int maxBytes = 512) {
    std::string out;
    FILE* f = fopen(path, "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof(buf), f) && (int)out.size() < maxBytes)
        out += buf;
    fclose(f);
    return out;
}

// Draw multiline text, return height used.
static int DrawMultiline(const char* s, int x, int y, int lineH, uint32_t col = 0x00FFFFFF) {
    int line = 0;
    const char* p = s;
    while (*p) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > 0 && len < 255) {
            char tmp[256]; memcpy(tmp, p, len); tmp[len] = 0;
            MT(tmp, x, y + line * lineH, col);
        }
        line++;
        if (!nl) break;
        p = nl + 1;
    }
    return line * lineH;
}

// ─── Frame wrap ──────────────────────────────────────────────────────────────

static void MenuBegin() {
    g_glRenderer->BeginFrame();
    g_glRenderer->ClearBuffers();
}
static void MenuEnd() {
    g_glRenderer->EndFrame();
}

// ─── Save file helpers ────────────────────────────────────────────────────────

struct PlayerSlot { bool exists; char name[128]; int score; int rank; };

static PlayerSlot ReadSlot(int n) {
    PlayerSlot ps = {};
    char fname[64]; wsprintf(fname, "trophy0%d.sav", n);
    HANDLE hf = CreateFileA(fname, GENERIC_READ, FILE_SHARE_READ,
                            NULL, OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return ps;
    TTrophyRoom tr = {}; DWORD l;
    ReadFile(hf, &tr, sizeof(tr), &l, NULL);
    CloseHandle(hf);
    ps.exists = true;
    strncpy(ps.name, tr.PlayerName, 127); ps.name[127] = 0;
    if (!ps.name[0]) strcpy(ps.name, "(unnamed)");
    ps.score = tr.Score; ps.rank = tr.Rank;
    return ps;
}

// ─── Player Select ───────────────────────────────────────────────────────────
// MENUR.TGA/MENUR_ON.TGA + MR_MAP.RAW
// Original layout (REGLISTX=320, REGLISTY=370 from Interface.cpp):
//   • Input box at top for typing new player name
//   • List of up to 5 existing players below (click to highlight)
//   • id=1 = OK (select highlighted or confirm new name)
//   • id=2 = DELETE (erase highlighted existing slot)
// Returns selected slot index, or -1 if aborted.

static void AppendChar(char* buf, int maxLen, int sc) {
    char ch = 0;
    bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        ch = (shift ? 'A' : 'a') + (sc - SDL_SCANCODE_A);
    else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) ch = '1' + (sc - SDL_SCANCODE_1);
    else if (sc == SDL_SCANCODE_0)     ch = '0';
    else if (sc == SDL_SCANCODE_SPACE) ch = ' ';
    if (ch && (int)strlen(buf) < maxLen - 1) {
        int n = (int)strlen(buf); buf[n] = ch; buf[n+1] = 0;
    }
}

static int RunPlayerSelect(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUR.TGA",
        "HUNTDAT\\MENU\\MENUR_ON.TGA",
        "HUNTDAT\\MENU\\MR_MAP.RAW");

    // Positions scaled from 800×600 originals
    // REGLISTX=320, REGLISTY=370 (from Interface.cpp)
    // Input box just above the list:
    int lx  = WinW * 307 / 800;   // list left x
    int iy  = WinH * 328 / 600;   // input box y
    int ly  = WinH * 368 / 600;   // list top y (REGLISTY)
    int slH = WinH * 32  / 600;   // slot height

    char typedName[32] = "";       // text being typed
    int  highlightSlot = -1;       // which existing slot is highlighted
    int  selected      = -1;
    bool newPlayer     = false;    // did we just create a brand-new player?

    while (selected < 0 && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        // Keyboard input
        int sc = gMI.scancode;
        if (sc == SDL_SCANCODE_BACKSPACE && typedName[0])
            typedName[strlen(typedName)-1] = 0;
        else if (sc == SDL_SCANCODE_ESCAPE)
            typedName[0] = 0;
        else if (sc && sc != SDL_SCANCODE_RETURN)
            AppendChar(typedName, 28, sc);

        int hov = CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);

        // Input box (for new name) — shown at top of register area
        {
            char disp[36]; wsprintf(disp, "%s_", typedName);
            MT(disp, lx, iy, 0x00FFFF80);
        }

        // Player list (5 slots)
        for (int i = 0; i < 5; i++) {
            PlayerSlot ps = ReadSlot(i);
            int sy = ly + i * slH;
            bool hot = gMI.x >= lx && gMI.x < lx + WinW*180/800 &&
                       gMI.y >= sy  && gMI.y < sy + slH;
            bool sel = (i == highlightSlot);

            if (ps.exists) {
                char line[160];
                wsprintf(line, "%s  %d", ps.name, ps.score);
                MTMed(line, lx, sy,
                      sel ? 0x00FFD040 : (hot ? 0x00FFFF80 : 0x00A0B0A0));
                if (hot && gMI.lClick) {
                    highlightSlot = i;
                    // SOURCEPORT: populate input field with clicked player's name
                    strncpy(typedName, ps.name, 31); typedName[31] = 0;
                }
            }
            // Empty slots are shown by the background image itself
        }

        // OK / DELETE via map buttons
        if (gMI.lClick) {
            if (hov == 1) {  // OK
                if (typedName[0]) {
                    // SOURCEPORT: if the typed name matches the highlighted existing slot, load it;
                    // otherwise treat the name as-typed as a new (or overwritten) player.
                    PlayerSlot hps = (highlightSlot >= 0) ? ReadSlot(highlightSlot) : PlayerSlot{};
                    if (highlightSlot >= 0 && hps.exists &&
                        _stricmp(typedName, hps.name) == 0) {
                        // Name unchanged — load existing player
                        TrophyRoom.RegNumber = highlightSlot;
                        LoadTrophy();
                        if (TrophyRoom.Score <= 0) { TrophyRoom.Score = 100; SaveTrophy(); }
                        selected = highlightSlot;
                    } else {
                        // New name or modified name — create player in first empty slot
                        int slot = -1;
                        for (int i = 0; i < 5; i++) { if (!ReadSlot(i).exists) { slot=i; break; } }
                        if (slot < 0) slot = highlightSlot >= 0 ? highlightSlot : 0;
                        memset(&TrophyRoom, 0, sizeof(TrophyRoom));
                        TrophyRoom.RegNumber = slot;
                        TrophyRoom.Score     = 100;  // starting account balance
                        strncpy(TrophyRoom.PlayerName, typedName, 127);
                        // Initialize game options to sensible defaults for new players
                        OptAgres = 128; OptDens = 100; OptSens = 128; OptViewR = 160;
                        SaveTrophy();
                        newPlayer = true;
                        selected  = slot;
                    }
                } else if (highlightSlot >= 0 && ReadSlot(highlightSlot).exists) {
                    TrophyRoom.RegNumber = highlightSlot;
                    LoadTrophy();
                    // Migrate saves created before Score was initialised to 100
                    if (TrophyRoom.Score <= 0) { TrophyRoom.Score = 100; SaveTrophy(); }
                    selected = highlightSlot;
                }
            } else if (hov == 2) {  // DELETE
                if (highlightSlot >= 0 && ReadSlot(highlightSlot).exists) {
                    char fname[64]; wsprintf(fname, "trophy0%d.sav", highlightSlot);
                    DeleteFileA(fname);
                    highlightSlot = -1;
                }
            }
        }
        if (sc == SDL_SCANCODE_RETURN && typedName[0]) {
            // SOURCEPORT: Enter key — load existing if name matches highlighted slot, else create
            PlayerSlot hps = (highlightSlot >= 0) ? ReadSlot(highlightSlot) : PlayerSlot{};
            if (highlightSlot >= 0 && hps.exists && _stricmp(typedName, hps.name) == 0) {
                TrophyRoom.RegNumber = highlightSlot;
                LoadTrophy();
                if (TrophyRoom.Score <= 0) { TrophyRoom.Score = 100; SaveTrophy(); }
                selected = highlightSlot;
            } else {
                int slot = -1;
                for (int i = 0; i < 5; i++) { if (!ReadSlot(i).exists) { slot=i; break; } }
                if (slot < 0) slot = highlightSlot >= 0 ? highlightSlot : 0;
                memset(&TrophyRoom, 0, sizeof(TrophyRoom));
                TrophyRoom.RegNumber = slot;
                TrophyRoom.Score     = 100;
                strncpy(TrophyRoom.PlayerName, typedName, 127);
                OptAgres = 128; OptDens = 100; OptSens = 128; OptViewR = 160;
                SaveTrophy();
                newPlayer = true;
                selected  = slot;
            }
        }

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);

    // Show waiver for brand-new players
    if (selected >= 0 && newPlayer) {
        // Waiver is shown inline in RunMenus using the same MENUR background
        // We signal this via the newPlayer flag captured in RunMenus
        // Store it in a file-scoped flag:
        extern bool gMenuNewPlayer;
        gMenuNewPlayer = true;
    }

    return selected;
}

// File-scope flag set by RunPlayerSelect, read by RunMenus
bool gMenuNewPlayer = false;

// ─── Waiver ──────────────────────────────────────────────────────────────────
// Shown after a new player is created.  Same MENUR background.
// id=1 = ACCEPT,  id=2 = DECLINE (goes back to player select)
// Returns true if accepted.

static bool RunWaiver(bool& appQuit) {
    // SOURCEPORT: use MENUL.TGA/MENUL_ON.TGA + ML_MAP.RAW for the waiver screen.
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUL.TGA",
        "HUNTDAT\\MENU\\MENUL_ON.TGA",
        "HUNTDAT\\MENU\\ML_MAP.RAW");

    bool decided = false;
    bool accepted = false;

    while (!decided && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);

        if (gMI.lClick) {
            if (hov == 1) { accepted = true;  decided = true; }
            if (hov == 2) { accepted = false; decided = true; }
        }

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);
    return accepted;
}

// ─── Main Menu ───────────────────────────────────────────────────────────────
// MENUM.TGA/MENUM_ON.TGA + MAIN_MAP.RAW
// Actual button order from screenshot:
//   id=1 HUNT  id=2 OPTIONS  id=3 TROPHY  id=4 CREDITS  id=5 QUIT
//   id=6 = NAME/ACCOUNT display area (top-right, not a menu action button)
// Return values: 0=Hunt 1=Trophy 2=Options 3=Credits 4=Quit 5=ChangePlayer

static int RunMainMenu(bool& appQuit) {
    MenuStartAmb();   // MENUM → MENUAMB
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUM.TGA",
        "HUNTDAT\\MENU\\MENUM_ON.TGA",
        "HUNTDAT\\MENU\\MAIN_MAP.RAW");

    int result = -99;

    while (result == -99 && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);

        MenuBegin();
        DrawMenuScreen(ms);

        // Overlay player name and score into the NAME/ACCOUNT bar baked into MENUM.TGA.
        // Positions are in 800×600 design space, scaled.
        // NAME bar spans ~x=0-100; ACCOUNT bar spans ~x=310-510.  Bars are ~26px tall.
        MTMed(TrophyRoom.PlayerName, WinW*98/800, WinH*12/600, 0x00AC6D24);
        {
            char scoreBuf[32]; wsprintf(scoreBuf, "%d", TrophyRoom.Score);
            MTMed(scoreBuf, WinW*478/800, WinH*12/600, 0x00AC6D24);
        }

        if (gMI.lClick) {
            switch (hov) {
            case 1: result = 0; break;   // HUNT
            case 2: result = 2; break;   // OPTIONS
            case 3: result = 1; break;   // TROPHY
            case 4: result = 3; break;   // CREDITS
            case 5: result = 4; break;   // QUIT
            // id=6 = name/account display area — not a navigation button
            }
        }
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) result = 4;

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);
    return result;
}

// ─── Quit Confirm ────────────────────────────────────────────────────────────
// MENUQ.TGA/MENUQ_ON.TGA + MQ_MAP.RAW
// IDs: 1=Yes, 2=No

static bool RunQuitConfirm(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUQ.TGA",
        "HUNTDAT\\MENU\\MENUQ_ON.TGA",
        "HUNTDAT\\MENU\\MQ_MAP.RAW");

    bool confirmed = false;
    bool decided   = false;

    while (!decided && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);

        MenuBegin();
        DrawMenuScreen(ms);

        if (gMI.lClick) {
            if (hov == 1) { confirmed = true;  decided = true; }
            if (hov == 2) { confirmed = false; decided = true; }
        }
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) { decided = true; }

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);
    return confirmed;
}

// ─── Area Select ─────────────────────────────────────────────────────────────
// MENUL.TGA/MENUL_ON.TGA + ML_MAP.RAW
// IDs: 1=Prev area, 2=Next area

static const int kNumAreas = 5;
static const char* kAreaFiles[kNumAreas] = {
    "HUNTDAT/AREAS/AREA1",
    "HUNTDAT/AREAS/AREA2",
    "HUNTDAT/AREAS/AREA3",
    "HUNTDAT/AREAS/AREA4",
    "HUNTDAT/AREAS/AREA5",
};
static const char* kAreaPics[kNumAreas] = {
    "HUNTDAT\\MENU\\PICS\\AREA1.TGA",
    "HUNTDAT\\MENU\\PICS\\AREA2.TGA",
    "HUNTDAT\\MENU\\PICS\\AREA3.TGA",
    "HUNTDAT\\MENU\\PICS\\AREA4.TGA",
    "HUNTDAT\\MENU\\PICS\\AREA5.TGA",
};
static const char* kAreaTxt[kNumAreas] = {
    "HUNTDAT\\MENU\\TXT\\AREA1.TXT",
    "HUNTDAT\\MENU\\TXT\\AREA2.TXT",
    "HUNTDAT\\MENU\\TXT\\AREA3.TXT",
    "HUNTDAT\\MENU\\TXT\\AREA4.TXT",
    "HUNTDAT\\MENU\\TXT\\AREA5.TXT",
};

// Returns selected area index (0-4), or -1 to go back.
static int RunAreaSelect(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUL.TGA",
        "HUNTDAT\\MENU\\MENUL_ON.TGA",
        "HUNTDAT\\MENU\\ML_MAP.RAW");

    TPicture areaPic = {};
    int curArea = 0;
    int result  = -99;   // -99 = still running

    auto loadAreaPic = [&]() {
        FreePic(areaPic);
        SafeLoadTGA(areaPic, kAreaPics[curArea]);
    };
    loadAreaPic();

    std::string areaText;
    auto loadAreaText = [&]() {
        areaText = ReadTextFile(kAreaTxt[curArea], 512);
    };
    loadAreaText();

    while (result == -99 && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);

        MenuBegin();
        DrawMenuScreen(ms);

        // Area preview image — placed in the info panel region of MENUL (roughly center-left)
        // MENUL image shows the area name/description panel; we overlay the area TGA there.
        // Approximate placement: center at (WinW*0.5, WinH*0.4), 357x208 scaled to 50% screen.
        if (areaPic.lpImage) {
            // Scale the preview to fit a reasonable area, roughly half the screen width
            int pw = (WinW * 60) / 100;
            int ph = (pw * areaPic.H) / areaPic.W;
            int px = (WinW - pw) / 2;
            int py = (WinH * 15) / 100;
            OverlayPic(areaPic, px, py, pw, ph);

            // Area text below the image
            const char* body = strchr(areaText.c_str(), '\n');
            if (body) body++;
            if (body) DrawMultiline(body, px, py + ph + 8, 16, 0x00A0C0A0);
        }

        // Area name header
        {
            char name[64] = "";
            const char* nl = strchr(areaText.c_str(), '\n');
            if (nl) { int l = (int)(nl - areaText.c_str()); if (l < 63) { memcpy(name, areaText.c_str(), l); name[l] = 0; } }
            if (!name[0]) wsprintf(name, "Area %d", curArea + 1);
            MT(name, WinW/2 - 80, WinH * 10 / 100, 0x00FFD040);
        }

        if (gMI.lClick) {
            if (hov == 1) { // Prev area
                curArea = (curArea + kNumAreas - 1) % kNumAreas;
                loadAreaPic(); loadAreaText();
            } else if (hov == 2) { // Next area; wrapping back to 0 confirms selection
                int next = (curArea + 1) % kNumAreas;
                if (next == 0) {
                    result = curArea; // wrapped — confirm current
                } else {
                    curArea = next;
                    loadAreaPic(); loadAreaText();
                }
            }
        }
        // Clicking the area image itself confirms
        {
            int pw = (WinW * 40) / 100;
            int ph = areaPic.H ? (pw * areaPic.H) / areaPic.W : 0;
            int px = (WinW - pw) / 2;
            int py = (WinH * 15) / 100;
            bool imgHot = areaPic.lpImage &&
                          gMI.x >= px && gMI.x < px+pw && gMI.y >= py && gMI.y < py+ph;
            if (imgHot && gMI.lClick) result = curArea;
        }
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) { result = -1; }

        MenuEnd();
        SDL_Delay(16);
    }

    FreePic(areaPic);
    FreeMenuScreen(ms);
    return result == -99 ? -1 : result;
}

// ─── Time of Day Select ───────────────────────────────────────────────────────
// MENU2.TGA/MENU2_ON.TGA (reused for time selection) — no dedicated screen.
// Use MENUL screen with text overlay as fallback.
// Returns 0=Dawn, 1=Day, 2=Night, or -1 to go back.

static int RunDaySelect(bool& appQuit) {
    MenuScreen ms = {};
    // MENU2 has tabs that include time-of-day; for simplicity reuse MENUD
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUD.TGA",
        "HUNTDAT\\MENU\\MENUD_ON.TGA",
        nullptr);  // no map needed — use custom text buttons

    static const char* kDayNames[3] = { "Dawn", "Day", "Night" };
    static const char* kDayDesc[3]  = {
        "HUNTDAT\\MENU\\TXT\\DAY1.NFO",
        "HUNTDAT\\MENU\\TXT\\DAY2.NFO",
        "HUNTDAT\\MENU\\TXT\\DAY3.NFO",
    };

    int sel = 1;   // default: Day
    int result = -99;

    while (result == -99 && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);

        MT("SELECT TIME OF DAY", WinW/2 - 80, WinH * 10 / 100, 0x00FFD040);

        // Three large buttons centered
        int btnW = 140, btnH = 40, gap = 20;
        int totalW = 3 * btnW + 2 * gap;
        int bx0 = WinW/2 - totalW/2;
        int by  = WinH * 40 / 100;

        for (int d = 0; d < 3; d++) {
            int bx = bx0 + d * (btnW + gap);
            bool hot  = gMI.x >= bx && gMI.x < bx+btnW &&
                        gMI.y >= by  && gMI.y < by+btnH;
            bool isSel = (sel == d);
            uint32_t bg = isSel ? 0xC0204000 : (hot ? 0x80304010 : 0x80101018);
            uint32_t bd = isSel ? 0xFF80FF40 : (hot ? 0xFF50A050 : 0xFF404060);
            g_glRenderer->FillRect(bx, by, btnW, btnH, bg);
            g_glRenderer->FillRect(bx, by, btnW, 1, bd);
            g_glRenderer->FillRect(bx, by+btnH-1, btnW, 1, bd);
            g_glRenderer->FillRect(bx, by, 1, btnH, bd);
            g_glRenderer->FillRect(bx+btnW-1, by, 1, btnH, bd);
            MT(kDayNames[d], bx + btnW/2 - 18, by + btnH/2 - 8,
               isSel ? 0x00FFFF40 : (hot ? 0x00FFD080 : 0x00C0C0C0));
            if (hot && gMI.lClick) sel = d;
        }

        // Description
        std::string desc = ReadTextFile(kDayDesc[sel], 256);
        DrawMultiline(desc.c_str(), WinW/2 - 200, by + btnH + 20, 18, 0x00A0C0A0);

        // Back/Next
        int nbx = WinW - 110, nby = WinH - 46;
        bool nHot = gMI.x >= nbx && gMI.x < nbx+90 && gMI.y >= nby && gMI.y < nby+24;
        g_glRenderer->FillRect(nbx-4, nby-4, 98, 32, nHot ? 0xFF304010 : 0xFF101018);
        MT("SELECT  >", nbx, nby, nHot ? 0x00FFFF40 : 0x00C8A050);
        if (nHot && gMI.lClick) result = sel;

        int bbx = 20, bby = WinH - 46;
        bool bHot = gMI.x >= bbx && gMI.x < bbx+80 && gMI.y >= bby && gMI.y < bby+24;
        g_glRenderer->FillRect(bbx-4, bby-4, 88, 32, bHot ? 0xFF301010 : 0xFF101018);
        MT("< BACK", bbx, bby, bHot ? 0x00FFD040 : 0x00808080);
        if (bHot && gMI.lClick) result = -1;

        if (gMI.scancode == SDL_SCANCODE_ESCAPE) result = -1;

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);
    return result == -99 ? -1 : result;
}

// ─── Dino Select ─────────────────────────────────────────────────────────────
// MENUD.TGA/MENUD_ON.TGA + MD_MAP.RAW
// IDs: 1=Prev, 2=Next
// Returns final TargetDino bitmask, or -1 to go back.

static const char* kDinoPics[9] = {
    "HUNTDAT\\MENU\\PICS\\DINO1.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO2.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO3.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO4.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO5.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO6.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO7.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO8.TGA",
    "HUNTDAT\\MENU\\PICS\\DINO9.TGA",
};
static const char* kDinoTxtM[9] = {
    "HUNTDAT\\MENU\\TXT\\DINO1.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO2.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO3.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO4.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO5.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO6.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO7.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO8.TXM",
    "HUNTDAT\\MENU\\TXT\\DINO9.TXM",
};

static int RunDinoSelect(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENUD.TGA",
        "HUNTDAT\\MENU\\MENUD_ON.TGA",
        "HUNTDAT\\MENU\\MD_MAP.RAW");

    // Start: all dinos enabled
    int mask = 0;
    for (int c = 0; c < TotalC && c < 9; c++)
        mask |= (1 << DinoInfo[c].AI);
    TargetDino = mask;

    int curDino = 0;  // currently viewed dino in the info panel
    TPicture dinoPic = {};
    auto loadDinoPic = [&]() {
        FreePic(dinoPic);
        if (curDino < 9) SafeLoadTGA(dinoPic, kDinoPics[curDino]);
    };
    loadDinoPic();

    int result = -99;

    while (result == -99 && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);

        MenuBegin();
        DrawMenuScreen(ms);

        // Dino preview
        if (dinoPic.lpImage && curDino < TotalC) {
            int pw = (WinW * 60) / 100;
            int ph = (pw * dinoPic.H) / dinoPic.W;
            int px = (WinW - pw) / 2;
            int py = (WinH * 14) / 100;
            OverlayPic(dinoPic, px, py, pw, ph);

            // Info text
            std::string info = (curDino < 9) ? ReadTextFile(kDinoTxtM[curDino], 256) : "";
            DrawMultiline(info.c_str(), px, py + ph + 6, 16, 0x00A0C0A0);
        }

        // Dino name header
        if (curDino < TotalC)
            MT(DinoInfo[curDino].Name, WinW/2 - 60, WinH * 10 / 100, 0x00FFD040);

        // Toggle button for current dino
        {
            bool en = (TargetDino & (1 << DinoInfo[curDino].AI)) != 0;
            int tx = WinW/2 - 50, ty = WinH * 88 / 100;
            bool tHot = gMI.x >= tx && gMI.x < tx+100 && gMI.y >= ty && gMI.y < ty+22;
            g_glRenderer->FillRect(tx-4, ty-4, 108, 30,
                en ? (tHot ? 0xFF304020 : 0xFF203010) : (tHot ? 0xFF302020 : 0xFF201010));
            MT(en ? "ENABLED" : "DISABLED", tx + 10, ty,
               en ? 0x0040FF40 : 0x00FF4040);
            if (tHot && gMI.lClick) {
                int bit = (1 << DinoInfo[curDino].AI);
                int newMask = TargetDino ^ bit;
                if (newMask != 0) TargetDino = newMask;
            }
        }

        // Nav buttons handled by map IDs (1=prev, 2=next; "DONE" overlay button confirms)
        if (gMI.lClick) {
            if (hov == 1) {  // Prev
                curDino = (curDino + TotalC - 1) % (TotalC > 0 ? TotalC : 1);
                loadDinoPic();
            } else if (hov == 2) {  // Next — cycles forward
                curDino = (curDino + 1) % (TotalC > 0 ? TotalC : 1);
                loadDinoPic();
            }
        }

        // Explicit NEXT/BACK overlay buttons
        {
            int nbx = WinW - 110, nby = WinH - 46;
            bool nHot = gMI.x >= nbx && gMI.x < nbx+90 && gMI.y >= nby && gMI.y < nby+24;
            g_glRenderer->FillRect(nbx-4, nby-4, 98, 32, nHot ? 0xFF304010 : 0xFF101018);
            MT("DONE  >", nbx, nby, nHot ? 0x00FFFF40 : 0x00C8A050);
            if (nHot && gMI.lClick) result = TargetDino;

            int bbx = 20, bby = WinH - 46;
            bool bHot = gMI.x >= bbx && gMI.x < bbx+80 && gMI.y >= bby && gMI.y < bby+24;
            g_glRenderer->FillRect(bbx-4, bby-4, 88, 32, bHot ? 0xFF301010 : 0xFF101018);
            MT("< BACK", bbx, bby, bHot ? 0x00FFD040 : 0x00808080);
            if (bHot && gMI.lClick) result = -1;
        }

        if (gMI.scancode == SDL_SCANCODE_ESCAPE) result = -1;

        MenuEnd();
        SDL_Delay(16);
    }

    FreePic(dinoPic);
    FreeMenuScreen(ms);
    return result;
}

// ─── Weapon / Equipment select ────────────────────────────────────────────────
// MENU2.TGA/MENU2_ON.TGA + M2_MAP.RAW
// Tab IDs 1-6 in the map = tabs for the hunt setup.
// We use this screen for weapons and equipment selection.

static const char* kWeaponPics[6] = {
    "HUNTDAT\\MENU\\PICS\\WEAPON1.TGA",
    "HUNTDAT\\MENU\\PICS\\WEAPON2.TGA",
    "HUNTDAT\\MENU\\PICS\\WEAPON3.TGA",
    "HUNTDAT\\MENU\\PICS\\WEAPON4.TGA",
    "HUNTDAT\\MENU\\PICS\\WEAPON5.TGA",
    "HUNTDAT\\MENU\\PICS\\WEAPON6.TGA",
};
static const char* kWeaponTxt[6] = {
    "HUNTDAT\\MENU\\TXT\\WEAPON1.TXT",
    "HUNTDAT\\MENU\\TXT\\WEAPON2.TXT",
    "HUNTDAT\\MENU\\TXT\\WEAPON3.TXT",
    "HUNTDAT\\MENU\\TXT\\WEAPON4.TXT",
    "HUNTDAT\\MENU\\TXT\\WEAPON5.TXT",
    "HUNTDAT\\MENU\\TXT\\WEAPON6.TXT",
};
static const char* kEquipPics[4] = {
    "HUNTDAT\\MENU\\PICS\\EQUIP1.TGA",
    "HUNTDAT\\MENU\\PICS\\EQUIP2.TGA",
    "HUNTDAT\\MENU\\PICS\\EQUIP3.TGA",
    "HUNTDAT\\MENU\\PICS\\EQUIP4.TGA",
};
static const char* kEquipNFO[4] = {
    "HUNTDAT\\MENU\\TXT\\CAMOFLAG.NFO",
    "HUNTDAT\\MENU\\TXT\\SCENT.NFO",
    "HUNTDAT\\MENU\\TXT\\RADAR.NFO",
    "HUNTDAT\\MENU\\TXT\\TRANQ.NFO",
};

// Returns true if confirmed, false if back.
static bool RunWeaponEquipSelect(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENU2.TGA",
        "HUNTDAT\\MENU\\MENU2_ON.TGA",
        "HUNTDAT\\MENU\\M2_MAP.RAW");

    // Load weapon pics
    TPicture wepPics[6] = {};
    for (int w = 0; w < TotalW && w < 6; w++)
        SafeLoadTGA(wepPics[w], kWeaponPics[w]);

    TPicture equipPics[4] = {};
    for (int e = 0; e < 4; e++)
        SafeLoadTGA(equipPics[e], kEquipPics[e]);

    // Reset all selections
    WeaponPres = 0;
    if (TotalW > 0) WeaponPres |= 1;       // default: pistol
    if (TotalW > 4) WeaponPres |= (1 << 4); // and rifle
    CamoMode = ScentMode = RadarMode = Tranq = DoubleAmmo = ObservMode = FALSE;

    // Tab state: 0=weapons, 1=equipment
    int activeTab = 0;
    int curWep    = 0;
    int curEquip  = 0;
    bool confirmed = false;
    bool decided   = false;

    BOOL* equipFlags[4] = { &CamoMode, &ScentMode, &RadarMode, &Tranq };
    static const char* kEquipNames[4] = { "Camouflage", "Scent mask", "Radar", "Tranquilizer" };

    while (!decided && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        int hov = CompositeMenu(ms);

        MenuBegin();
        DrawMenuScreen(ms);

        // M2_MAP IDs:
        //   1-6  = weapon tabs (6 weapons), each ~70px on left half then ~200px on right half
        //   7    = Back button (bottom left)
        //   8    = Next/Start Hunt button (bottom right)
        if (gMI.lClick) {
            if (hov == 7) { decided = true; confirmed = false; }  // Back
            if (hov == 8) { decided = true; confirmed = true;  }  // Start Hunt
            // Tabs 1-6 switch weapon view OR jump to equipment tab
            if (hov >= 1 && hov <= 5 && activeTab == 0) { curWep = hov - 1; }
            if (hov == 6) { activeTab = (activeTab == 1) ? 0 : 1; }  // toggle equip tab
        }
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) { decided = true; confirmed = false; }

        // Draw tab labels at the M2_MAP positions (map-space × 2 → screen-space)
        // tab x boundaries (screen): 0,70,132,202,402,602,800
        {
            static const int tabXsScreen[] = { 0, 70, 132, 202, 402, 602 };
            static const int tabWidths[]   = { 70, 62, 70, 200, 200, 198 };
            static const char* tabLabels[] = { "W1","W2","W3","W4","W5","EQUIP" };
            int tabY = (WinH * 288) / 600;
            for (int t = 0; t < 6; t++) {
                int sx = (WinW * tabXsScreen[t]) / 800;
                int sw = (WinW * tabWidths[t])   / 800;
                bool tabHot = (hov == t+1);
                bool tabSel = (activeTab == 0 && t < 5 && t == curWep) ||
                              (activeTab == 1 && t == 5);
                MT(tabLabels[t], sx + 4, tabY,
                   tabHot ? 0x00FFFF40 : (tabSel ? 0x00FFD040 : 0x00C0C0C0));
                (void)sw;
            }
        }

        // Content area: tabs are at screen y≈322, buttons at y≈552. Use middle.
        // Map coords: tabs y=144-161, back/next y=276-295; content between 162-275.
        int contentY = (WinH * 325) / 600;

        if (activeTab == 0) {
            // Weapons: show current weapon image + info, toggle on/off
            if (curWep < TotalW && curWep < 6 && wepPics[curWep].lpImage) {
                int pw = (WinW * 45) / 100;
                int ph = (pw * wepPics[curWep].H) / wepPics[curWep].W;
                int px = WinW/2 - pw/2;
                OverlayPic(wepPics[curWep], px, contentY, pw, ph);

                std::string txt = ReadTextFile(kWeaponTxt[curWep < 6 ? curWep : 0], 256);
                DrawMultiline(txt.c_str(), px, contentY + ph + 6, 15, 0x00A0C0A0);
            }

            // Weapon name header
            if (curWep < TotalW)
                MT(WeapInfo[curWep].Name, WinW/2 - 40, contentY - 22, 0x00FFD040);

            // Toggle selected/not for this weapon
            bool en = curWep < TotalW && (WeaponPres & (1 << curWep)) != 0;
            {
                int tx = WinW/2 - 55, ty = contentY - 22;
                bool hot = gMI.x >= tx && gMI.x < tx+110 && gMI.y >= ty && gMI.y < ty+18;
                g_glRenderer->FillRect(tx-4, ty-4, 118, 26, en ? 0xFF203010 : 0xFF201010);
                MT(en ? "SELECTED" : "NOT SELECTED", tx + 2, ty,
                   en ? 0x0060FF60 : 0x00FF4040);
                if (hot && gMI.lClick && curWep < TotalW) {
                    int bit = (1 << curWep);
                    int newMask = WeaponPres ^ bit;
                    if (newMask != 0) WeaponPres = newMask;
                }
            }
        } else {
            // Equipment: show current equip image + info, toggle on/off
            if (curEquip < 4 && equipPics[curEquip].lpImage) {
                int pw = (WinW * 45) / 100;
                int ph = (pw * equipPics[curEquip].H) / equipPics[curEquip].W;
                int px = WinW/2 - pw/2;
                OverlayPic(equipPics[curEquip], px, contentY, pw, ph);

                std::string txt = ReadTextFile(kEquipNFO[curEquip], 256);
                DrawMultiline(txt.c_str(), px, contentY + ph + 6, 15, 0x00A0C0A0);
            }

            // Equipment: show image for currently-highlighted equip item
            MT(kEquipNames[curEquip], WinW/2 - 50, contentY - 22, 0x00FFD040);

            // Four toggle buttons in a row for all equipment
            {
                int btnW = 140, gap = 10;
                int total = 4 * btnW + 3 * gap;
                int bx0 = WinW/2 - total/2;
                int by  = contentY + 5;
                for (int e = 0; e < 4; e++) {
                    int bx = bx0 + e * (btnW + gap);
                    bool en2  = *equipFlags[e] != FALSE;
                    bool hot  = gMI.x >= bx && gMI.x < bx+btnW &&
                                gMI.y >= by  && gMI.y < by+28;
                    bool isCur = (e == curEquip);
                    g_glRenderer->FillRect(bx, by, btnW, 28,
                        en2 ? 0xFF203010 : 0xFF201010);
                    if (isCur)
                        g_glRenderer->FillRect(bx, by, btnW, 1, 0xFF80FFA0);
                    MT(kEquipNames[e], bx + 4, by + 5,
                       en2 ? 0x0060FF60 : 0x00808080);
                    if (hot && gMI.lClick) {
                        curEquip = e;
                        *equipFlags[e] = *equipFlags[e] ? FALSE : TRUE;
                    }
                }
            }

            // Equipment description + image below the toggle bar
            {
                int by = contentY + 40;
                if (curEquip < 4 && equipPics[curEquip].lpImage) {
                    int pw = (WinW * 30) / 100;
                    int ph = (pw * equipPics[curEquip].H) / equipPics[curEquip].W;
                    int px = WinW/2 - pw/2;
                    OverlayPic(equipPics[curEquip], px, by, pw, ph);
                    std::string txt = ReadTextFile(kEquipNFO[curEquip], 256);
                    DrawMultiline(txt.c_str(), px, by + ph + 6, 15, 0x00A0C0A0);
                }
            }
        }

        MenuEnd();
        SDL_Delay(16);
    }

    for (int w = 0; w < 6; w++) FreePic(wepPics[w]);
    for (int e = 0; e < 4; e++) FreePic(equipPics[e]);
    FreeMenuScreen(ms);
    return confirmed;
}

// ─── Options screen ───────────────────────────────────────────────────────────
// OPT_OFF.TGA / OPT_ON.TGA + OPT_MAP.RAW
// Layout: GAME (top-left panel), VIDEO (bottom-left panel), CONTROLS (right).
// OPT_MAP ids: 1=GAME icon, 2=CONTROLS icon, 3=VIDEO icon, 4=BACK

static void RunOptions(bool& appQuit) {
    MenuStartAmb();   // OPT → MENUAMB
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\OPT_OFF.TGA",
        "HUNTDAT\\MENU\\OPT_ON.TGA",
        "HUNTDAT\\MENU\\OPT_MAP.RAW");

    // VK → readable name
    auto VKStr = [](int vk, char* buf, int sz) {
        if (vk == VK_LBUTTON)  { strncpy(buf, "Mouse1", sz); return; }
        if (vk == VK_RBUTTON)  { strncpy(buf, "Mouse2", sz); return; }
        if (vk == VK_SPACE)    { strncpy(buf, "Space",  sz); return; }
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT)
                               { strncpy(buf, "Shift",  sz); return; }
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
                               { strncpy(buf, "Ctrl",   sz); return; }
        if (vk == VK_MENU || vk == VK_LMENU) { strncpy(buf, "Alt", sz); return; }
        if (vk == VK_UP)       { strncpy(buf, "Up",     sz); return; }
        if (vk == VK_DOWN)     { strncpy(buf, "Down",   sz); return; }
        if (vk == VK_LEFT)     { strncpy(buf, "Left",   sz); return; }
        if (vk == VK_RIGHT)    { strncpy(buf, "Right",  sz); return; }
        if (vk >= 'A' && vk <= 'Z') { buf[0]=(char)vk; buf[1]=0; return; }
        if (vk >= '0' && vk <= '9') { buf[0]=(char)vk; buf[1]=0; return; }
        if (vk == 0) { strncpy(buf, "---", sz); return; }
        UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
        bool ext = (vk==VK_UP||vk==VK_DOWN||vk==VK_LEFT||vk==VK_RIGHT||
                    vk==VK_INSERT||vk==VK_DELETE||vk==VK_HOME||vk==VK_END||
                    vk==VK_PRIOR||vk==VK_NEXT);
        LONG lp = (LONG)(sc << 16) | (ext ? (1<<24) : 0);
        if (!GetKeyNameTextA(lp, buf, sz)) strncpy(buf, "?", sz);
    };

    TPicture slBar = {}, slBut = {};
    SafeLoadTGA(slBar, "HUNTDAT\\MENU\\SL_BAR.TGA");
    SafeLoadTGA(slBut, "HUNTDAT\\MENU\\SL_BUT.TGA");

    // Draw horizontal slider using SL_BAR.TGA / SL_BUT.TGA; returns updated value on click.
    auto DrawSlider = [&](int tx, int ty, int tw, int th,
                          int val, int minVal, int maxVal) -> int {
        int range = maxVal - minVal;
        // Bar background
        if (slBar.lpImage && slBar.W > 0)
            g_glRenderer->DrawBitmap(tx, ty, tw, th, slBar.W, slBar.lpImage, false, slBar.H);
        else
            g_glRenderer->FillRect(tx, ty, tw, th, 0xFF302820);

        if (range > 0) {
            // Thumb: scale SL_BUT to bar height, constrain to stay fully inside bar
            int butH   = (slBut.H > 0) ? slBut.H * th / std::max(1, slBar.H) : th + 4;
            int butW   = (slBut.W > 0) ? slBut.W * butH / std::max(1, slBut.H) : 8;
            int travel = tw - butW;
            int thumbX = tx + (travel > 0 ? (val - minVal) * travel / range : 0);
            int thumbY = ty + th / 2 - butH / 2;
            if (slBut.lpImage && slBut.W > 0)
                g_glRenderer->DrawBitmap(thumbX, thumbY, butW, butH, slBut.W, (void*)slBut.lpImage, true, slBut.H);
            else
                g_glRenderer->FillRect(thumbX, thumbY, butW, butH + 4, 0xFFC09060);
        }
        if (gMI.lHeld && gMI.x >= tx && gMI.x < tx+tw &&
                         gMI.y >= ty-4 && gMI.y < ty+th+4) {
            int butH   = (slBut.H > 0) ? slBut.H * th / std::max(1, slBar.H) : th + 4;
            int butW   = (slBut.W > 0) ? slBut.W * butH / std::max(1, slBut.H) : 8;
            int travel = tw - butW;
            int v = (travel > 0) ? minVal + (gMI.x - tx) * range / travel : minVal;
            return std::max(minVal, std::min(v, maxVal));
        }
        return val;
    };

    // Key rebinding state: waitIdx = index into bindings[] being rebound (-1 = none)
    struct Binding { const char* name; int* vk; };
    Binding bindings[] = {
        { "Forward",     &KeyMap.fkForward  },
        { "Backward",    &KeyMap.fkBackward },
        { "Turn Up",     &KeyMap.fkUp       },
        { "Turn Down",   &KeyMap.fkDown     },
        { "Turn Left",   &KeyMap.fkLeft     },
        { "Turn Right",  &KeyMap.fkRight    },
        { "Fire",        &KeyMap.fkFire     },
        { "Get weapon",  &KeyMap.fkShow     },
        { "Step Left",   &KeyMap.fkSLeft    },
        { "Step Right",  &KeyMap.fkSRight   },
        { "Strafe",      &KeyMap.fkStrafe   },
        { "Jump",        &KeyMap.fkJump     },
        { "Run",         &KeyMap.fkRun      },
        { "Crouch",      &KeyMap.fkCrouch   },
        { "Call",        &KeyMap.fkCall     },
        { "Change Call", &KeyMap.fkCCall    },
        { "Binoculars",  &KeyMap.fkBinoc    },
    };
    const int kNumBindings = (int)(sizeof(bindings)/sizeof(bindings[0]));
    int waitIdx = -1;   // index of binding waiting for a key press

    // Convert SDL scancode → Windows VK (for rebinding)
    auto ScancodeToVK = [](SDL_Scancode sc) -> int {
        if (sc == SDL_SCANCODE_SPACE)     return VK_SPACE;
        if (sc == SDL_SCANCODE_LSHIFT || sc == SDL_SCANCODE_RSHIFT)  return VK_SHIFT;
        if (sc == SDL_SCANCODE_LCTRL  || sc == SDL_SCANCODE_RCTRL)   return VK_CONTROL;
        if (sc == SDL_SCANCODE_LALT   || sc == SDL_SCANCODE_RALT)    return VK_MENU;
        if (sc == SDL_SCANCODE_UP)        return VK_UP;
        if (sc == SDL_SCANCODE_DOWN)      return VK_DOWN;
        if (sc == SDL_SCANCODE_LEFT)      return VK_LEFT;
        if (sc == SDL_SCANCODE_RIGHT)     return VK_RIGHT;
        // Letters and digits: SDL scan codes map cleanly via VK
        SDL_Keycode kc = SDL_GetKeyFromScancode(sc);
        if (kc >= SDLK_a && kc <= SDLK_z) return (int)('A' + (kc - SDLK_a));
        if (kc >= SDLK_0 && kc <= SDLK_9) return (int)('0' + (kc - SDLK_0));
        return 0;  // unsupported — ignore
    };

    while (!appQuit) {
        // Custom event poll so we can intercept raw key events for rebinding
        gMI.lClick = false; gMI.rClick = false; gMI.scancode = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:   appQuit = true; break;
            case SDL_MOUSEMOTION:   ScaleMouse(ev.motion.x, ev.motion.y); break;
            case SDL_MOUSEBUTTONDOWN:
                ScaleMouse(ev.button.x, ev.button.y);
                if (ev.button.button == SDL_BUTTON_LEFT)  { gMI.lClick = true; gMI.lHeld = true; }
                if (ev.button.button == SDL_BUTTON_RIGHT) gMI.rClick = true;
                // While waiting for a key: mouse buttons are also valid bindings
                if (waitIdx >= 0) {
                    int vk = (ev.button.button == SDL_BUTTON_LEFT)  ? VK_LBUTTON :
                             (ev.button.button == SDL_BUTTON_RIGHT) ? VK_RBUTTON : 0;
                    if (vk) { *bindings[waitIdx].vk = vk; waitIdx = -1; gMI.lClick = false; gMI.lHeld = false; }
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT) gMI.lHeld = false;
                break;
            case SDL_KEYDOWN:
                if (waitIdx >= 0) {
                    // Escape cancels rebind; any other key sets it
                    if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        waitIdx = -1;
                    } else {
                        int vk = ScancodeToVK(ev.key.keysym.scancode);
                        if (vk) { *bindings[waitIdx].vk = vk; waitIdx = -1; }
                    }
                } else {
                    gMI.scancode = ev.key.keysym.scancode;
                }
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    int dw, dh;
                    SDL_GL_GetDrawableSize(g_glRenderer->GetWindow(), &dw, &dh);
                    if (dw > 0 && dh > 0) {
                        extern void SetVideoMode(int, int);
                        SetVideoMode(dw, dh);
                        glViewport(0, 0, dw, dh);
                    }
                }
                break;
            }
        }
        if (appQuit) break;
        if (waitIdx < 0 && gMI.scancode == SDL_SCANCODE_ESCAPE) break;

        int hov = CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);

        // Layout (800×600 design space, scaled)
        int lnH  = WinH * 22 / 600;
        int lblW = WinW * 125 / 800;
        int slW  = WinW * 145 / 800;

        // ── GAME panel ────────────────────────────────────────────────────
        {
            int ox = WinW * 80 / 800;
            int y  = WinH * 115 / 600;

            // Ensure sensible defaults (globals start at 0 if never saved)
            if (OptAgres <= 0) OptAgres = 128;   // 128 = baseline dino health (x1.0)
            if (OptDens  <= 0) OptDens  = 100;   // mid-range spawn density
            if (OptSens  <= 0) OptSens  = 128;   // 128 = neutral detection cone
            if (OptViewR <= 0) OptViewR = 160;   // comfortable fog distance

            struct { const char* lbl; int* var; int mn; int mx; } sliders[] = {
                { "Agressivity", &OptAgres, 1, 255 },   // health multiplier: val/128 × base
                { "Density",     &OptDens,  1, 255 },   // spawn count: ~5+val/80 dinos
                { "Sensitivity", &OptSens,  1, 255 },   // detection cone: wider = easier detect
                { "View range",  &OptViewR, 1, 255 },   // fog range: ctViewR = 42+(val/8)*2
            };
            for (auto& s : sliders) {
                MTMed(s.lbl, ox, y, 0x00AC6D24);
                *s.var = DrawSlider(ox + lblW, y + 2, slW, lnH - 6, *s.var, s.mn, s.mx);
                y += lnH;
            }
            // Measurement toggle
            {
                int vx = ox + lblW;
                bool hot = gMI.x >= vx && gMI.x < vx+WinW*80/800 && gMI.y >= y && gMI.y < y+lnH;
                MTMed("Measurement", ox, y, 0x00AC6D24);
                MT(OptSys==0 ? "Metric" : "US", vx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                if (hot && gMI.lClick) OptSys = 1 - OptSys;
            }
        }

        // ── VIDEO panel ───────────────────────────────────────────────────
        {
            // SOURCEPORT: resolution presets (widescreen + classic 4:3), filtered to ≤ desktop
            struct ResPreset { int w, h; };
            static const ResPreset kPresets[] = {
                {  640,  480 }, {  800,  600 }, { 1024,  768 },
                { 1280,  720 }, { 1280,  960 }, { 1366,  768 },
                { 1600,  900 }, { 1920, 1080 },
                { 2560, 1440 }, { 3840, 2160 },
            };
            static const int kNumPresets = (int)(sizeof(kPresets)/sizeof(kPresets[0]));
            static const char* kModeNames[] = { "Windowed", "Fullscreen", "Borderless" };

            // Build filtered list once (preset indices whose dimensions fit the monitor).
            // SOURCEPORT: use SDL_GetNumDisplayModes/SDL_GetDisplayMode to find the
            // monitor's true maximum resolution — SDL_GetDesktopDisplayMode returns the
            // *current* desktop mode which may be an exclusive fullscreen mode lower than
            // the panel's native resolution, causing the list to be truncated.
            static int filtIdx[12];
            static int nFilt = 0;
            static bool presetsBuilt = false;
            if (!presetsBuilt) {
                int maxW = 640, maxH = 480;
                int numModes = SDL_GetNumDisplayModes(0);
                for (int mi = 0; mi < numModes; mi++) {
                    SDL_DisplayMode mode = {};
                    if (SDL_GetDisplayMode(0, mi, &mode) == 0)
                        if (mode.w * mode.h > maxW * maxH) { maxW = mode.w; maxH = mode.h; }
                }
                if (maxW == 640) {  // fallback: SDL_GetNumDisplayModes failed
                    SDL_DisplayMode dm = {};
                    SDL_GetDesktopDisplayMode(0, &dm);
                    if (dm.w > maxW) { maxW = dm.w; maxH = dm.h; }
                }
                for (int i = 0; i < kNumPresets; i++)
                    if (kPresets[i].w <= maxW && kPresets[i].h <= maxH)
                        filtIdx[nFilt++] = i;
                if (nFilt == 0) { filtIdx[0] = 0; nFilt = 1; }
                presetsBuilt = true;
            }

            // Find which filtered slot matches the current window size
            auto FindResIdx = [&]() -> int {
                for (int i = 0; i < nFilt; i++)
                    if (kPresets[filtIdx[i]].w == WinW && kPresets[filtIdx[i]].h == WinH)
                        return i;
                return nFilt - 1;
            };

            // Apply a resolution + display-mode combination immediately
            auto ApplyDisplay = [&](int ri, int mode) {
                SDL_Window* win = g_glRenderer->GetWindow();
                int tw = kPresets[filtIdx[ri]].w, th = kPresets[filtIdx[ri]].h;
                OptDisplayMode = mode; OptResW = tw; OptResH = th;
                extern void SetVideoMode(int, int);
                if (mode == 2) {                          // borderless fullscreen — always desktop res
                    SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    // Actual drawable size is the desktop; query it now
                    int dw, dh;
                    SDL_GL_GetDrawableSize(win, &dw, &dh);
                    if (dw > 0 && dh > 0) { tw = dw; th = dh; }
                } else if (mode == 1) {                   // exclusive fullscreen
                    // Explicitly request the desired display mode before entering fullscreen
                    // so SDL doesn't silently fall back to a different resolution.
                    SDL_DisplayMode desired = {};
                    desired.w = tw; desired.h = th;
                    SDL_SetWindowDisplayMode(win, &desired);
                    SDL_SetWindowFullscreen(win, 0);      // leave current mode first
                    SDL_SetWindowSize(win, tw, th);
                    SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN);
                    // Pump events to let the OS complete the async mode change, then
                    // read back the actual drawable so WinW/WinH are always correct.
                    SDL_PumpEvents();
                    int dw, dh; SDL_GL_GetDrawableSize(win, &dw, &dh);
                    if (dw > 0 && dh > 0) { tw = dw; th = dh; }
                } else {                                  // windowed
                    SDL_SetWindowFullscreen(win, 0);
                    SDL_SetWindowSize(win, tw, th);
                    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    // For windowed, drawable may differ on HiDPI
                    int dw, dh;
                    SDL_GL_GetDrawableSize(win, &dw, &dh);
                    if (dw > 0 && dh > 0) { tw = dw; th = dh; }
                }
                SetVideoMode(tw, th);
            };

            int ox  = WinW * 80 / 800;
            int y   = WinH * 350 / 600;
            int vx  = ox + lblW;
            int tvW = WinW * 130 / 800;  // clickable value column width

            // Audio / video driver (read-only)
            MTMed("Audio Driver", ox, y, 0x00AC6D24); MT("OpenAL Soft",  vx, y, 0x00C0C0C0); y += lnH;
            MTMed("Video Driver", ox, y, 0x00AC6D24); MT("OpenGL 3.3",  vx, y, 0x00C0C0C0); y += lnH;

            // Resolution — cycle presets for windowed/fullscreen; fixed at desktop for borderless
            {
                char resBuf[32];
                MTMed("Resolution", ox, y, 0x00AC6D24);
                if (OptDisplayMode == 2) {
                    // Borderless always uses the desktop resolution; not user-selectable
                    wsprintf(resBuf, "%dx%d", WinW, WinH);
                    MT(resBuf, vx, y, 0x00606060);
                } else {
                    int ri = FindResIdx();
                    wsprintf(resBuf, "%dx%d", kPresets[filtIdx[ri]].w, kPresets[filtIdx[ri]].h);
                    bool hot = gMI.x >= vx && gMI.x < vx+tvW && gMI.y >= y && gMI.y < y+lnH;
                    MT(resBuf, vx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                    if (hot && gMI.lClick) ApplyDisplay((ri + 1) % nFilt, OptDisplayMode);
                }
            }
            y += lnH;

            // Display mode — Windowed / Fullscreen / Borderless
            {
                bool hot = gMI.x >= vx && gMI.x < vx+tvW && gMI.y >= y && gMI.y < y+lnH;
                MTMed("Display", ox, y, 0x00AC6D24);
                MT(kModeNames[OptDisplayMode], vx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                if (hot && gMI.lClick) ApplyDisplay(FindResIdx(), (OptDisplayMode + 1) % 3);
            }
            y += lnH;

            // VSync toggle
            {
                bool hot = gMI.x >= vx && gMI.x < vx+tvW && gMI.y >= y && gMI.y < y+lnH;
                MTMed("VSync", ox, y, 0x00AC6D24);
                MT(OptVSync ? "On" : "Off", vx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                if (hot && gMI.lClick) {
                    OptVSync = 1 - OptVSync;
                    if (OptVSync) { if (SDL_GL_SetSwapInterval(-1) < 0) SDL_GL_SetSwapInterval(1); }
                    else SDL_GL_SetSwapInterval(0);
                }
            }
            y += lnH;

            // 3D Shadows / Fog toggles
            auto Toggle = [&](const char* lbl, BOOL& flag) {
                bool hot = gMI.x >= vx && gMI.x < vx+tvW && gMI.y >= y && gMI.y < y+lnH;
                MTMed(lbl, ox, y, 0x00AC6D24);
                MT(flag ? "On" : "Off", vx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                if (hot && gMI.lClick) flag = !flag;
                y += lnH;
            };
            Toggle("3D Shadows", SHADOWS3D);
            Toggle("Fog",        FOGENABLE);

            // Brightness slider — SOURCEPORT: live shader uniform, slider centre = neutral
            MTMed("Brightness", ox, y, 0x00AC6D24);
            {
                int prev = OptBrightness;
                OptBrightness = DrawSlider(vx, y+2, slW, lnH-6, OptBrightness+128, 64, 256) - 128;
                if (OptBrightness != prev)
                    g_glRenderer->SetBrightness(1.0f + OptBrightness / 128.0f);
            }
        }

        // ── CONTROLS panel ────────────────────────────────────────────────
        {
            char kbuf[32];
            int ox   = WinW * 450 / 800;   // action label left edge
            int kx   = WinW * 550 / 800;   // key name column
            int y    = WinH * 80 / 600;
            int lnH2 = WinH * 24 / 600;

            for (int i = 0; i < kNumBindings; i++) {
                bool waiting = (waitIdx == i);
                bool hot = !waiting && gMI.x >= kx && gMI.x < kx+WinW*100/800 &&
                           gMI.y >= y && gMI.y < y+lnH2;

                MTMed(bindings[i].name, ox, y, waiting ? 0x00FFFFFF : 0x00AC6D24);
                if (waiting) {
                    MT("Press key...", kx, y, 0x00FFFF00);
                } else {
                    VKStr(*bindings[i].vk, kbuf, sizeof(kbuf));
                    MT(kbuf, kx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                    if (hot && gMI.lClick) { waitIdx = i; gMI.lClick = false; }
                }
                y += lnH2;
                if (y > WinH * 530 / 600) break;
            }

            // Reverse mouse
            {
                bool hot = waitIdx < 0 && gMI.x >= kx && gMI.x < kx+WinW*60/800 &&
                           gMI.y >= y && gMI.y < y+lnH2;
                MTMed("Reverse mouse", ox, y, 0x00AC6D24);
                MT(REVERSEMS ? "On" : "Off", kx, y, hot ? 0x00FFFF40 : 0x00C0C0C0);
                if (hot && gMI.lClick) REVERSEMS = !REVERSEMS;
                y += lnH2;
            }
            // Mouse sensitivity
            MTMed("Mouse sensitivity", ox, y, 0x00AC6D24);
            OptMsSens = DrawSlider(kx, y+2, WinW*130/800, lnH2-6, OptMsSens, 1, 20);
        }

        // BACK
        if (waitIdx < 0 && gMI.lClick && hov == 4) break;

        MenuEnd();
        SDL_Delay(16);
    }

    FreePic(slBar);
    FreePic(slBut);
    SaveTrophy();
    SaveDisplayConfig(); // SOURCEPORT: persist display/graphics settings globally
    FreeMenuScreen(ms);
}

// ─── Trophy Room ──────────────────────────────────────────────────────────────

static void RunTrophyRoom(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms, "HUNTDAT\\MENU\\TROPHY_G.TGA", nullptr, nullptr);

    static const char* kRanks[] = { "Novice", "Hunter", "Master Hunter" };

    while (!appQuit) {
        if (!PollMenuEvents(appQuit)) break;
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) break;

        CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);

        char buf[128];
        wsprintf(buf, "Hunter: %s  Score: %d  Rank: %s",
            TrophyRoom.PlayerName, TrophyRoom.Score,
            TrophyRoom.Rank < 3 ? kRanks[TrophyRoom.Rank] : "Legend");
        MT(buf, 20, 20, 0x00FFD080);

        int ly = 50;
        bool anyTrophy = false;
        for (int t = 0; t < 23; t++) {
            if (!TrophyRoom.Body[t].ctype) continue;
            int ctype = TrophyRoom.Body[t].ctype;
            if (ctype < 0 || ctype >= 32) continue;
            anyTrophy = true;
            char line[128];
            wsprintf(line, "%s   Score: %d",
                DinoInfo[ctype].Name, TrophyRoom.Body[t].score);
            MT(line, 30, ly, 0x00C0D0C0);
            ly += 18;
            if (ly > WinH - 60) break;
        }
        if (!anyTrophy) MT("(No trophies yet)", WinW/2 - 70, WinH/2, 0x00606060);

        int bx = WinW/2 - 25, by = WinH - 40;
        bool hot = gMI.x >= bx && gMI.x < bx+50 && gMI.y >= by && gMI.y < by+20;
        g_glRenderer->FillRect(bx-4, by-4, 58, 28, hot ? 0xFF303020 : 0xFF181818);
        MT("BACK", bx, by, hot ? 0x00FFFF40 : 0x00C8A050);
        if (hot && gMI.lClick) break;

        MenuEnd();
        SDL_Delay(16);
    }

    FreeMenuScreen(ms);
}

// ─── Hunt Setup — MENU2 comprehensive single screen ──────────────────────────
// MENU2.TGA/MENU2_ON.TGA + M2_MAP.RAW
//
// Layout matches original Carnivores 2 MENU2 background art (800×600 design):
//   y=  0-288  TOP: VIEW frame (left), ACCOUNT section (centre), INFO text (right)
//   y=288-325  TABS: [DAWN|DAY|NIGHT] (left panel) | [TRANQUILIZER] | [OBSERVER MODE]
//   y=325-560  LISTS: each panel 200px wide — LOCATIONS|DINOSAURS|WEAPONS|EQUIPMENT
//   y=560-600  BOTTOM: BACK (map id=7), HUNT (map id=8) — labels in background art
//
// Background art already draws all panel headers and tab labels; we only overlay
// list item text, selection indicators, and the dynamic account/cost numbers.

static bool RunHuntSetup(bool& appQuit) {
    MenuStartAmb();   // MENU2 → MENUAMB
    MenuScreen ms = {};
    LoadMenuScreen(ms,
        "HUNTDAT\\MENU\\MENU2.TGA",
        "HUNTDAT\\MENU\\MENU2_ON.TGA",
        "HUNTDAT\\MENU\\M2_MAP.RAW");

    // ── Costs (original Carnivores 2 values) ──────────────────────────────
    static const char* kAreaNames[5] = {
        "Delphaeus Hills", "Fort Ciskin", "Vengar Fjords", "Manya Jungle", "Mount Ravan"
    };
    static const int kAreaCost[5] = { 20,  50,  100, 150, 200 };
    static const int kWepCost[6]  = { 20, 100,  150,  50, 100, 200 };
    static const char* kWepNames[6] = { "Pistol", "Shotgun", "DB Shotgun", "X-Bow", "Rifle", "Sniper Rifle" };
    // Pistol=20, Shotgun=100, DB Shotgun=150, X-Bow=50, Rifle=100, Sniper=200

    // ── Session-persistent state (defaults on first call per exe run) ────
    // Globals (TargetDino, WeaponPres, CamoMode, etc.) persist naturally;
    // local vars are mirrored into statics so they survive between hunts.
    static bool s_firstRun = true;
    static int  s_curArea  = 0;
    static int  s_curDay   = 1;
    static bool s_tranqOn  = false;
    static bool s_observOn = false;

    if (s_firstRun) {
        s_firstRun = false;
        // Reset everything to defaults the very first time this session
        s_curArea  = 0;
        s_curDay   = 1;
        s_tranqOn  = false;
        s_observOn = false;
        TargetDino = (1 << AI_PARA);
        WeaponPres = 1;
        CamoMode = RadarMode = ScentMode = DoubleAmmo = ObservMode = FALSE;
        Tranq = FALSE;
    }

    int  curArea  = s_curArea;
    int  curDay   = s_curDay;
    bool tranqOn  = s_tranqOn;
    bool observOn = s_observOn;

    // Equipment: Camouflage, Radar, Cover scent, Double Ammo (Tranq is a tab toggle)
    BOOL* equipFlags[4] = { &CamoMode, &RadarMode, &ScentMode, &DoubleAmmo };
    static const char* kEquipNms[4] = { "Camouflage", "Radar", "Cover scent", "Double Ammo" };

    // Dino species names and trophy scores, indexed by AI type (0-8)
    static const char* kDinoSpecies[9] = {
        "Parasaurolophus", "Ankylosaurus", "Stegosaurus", "Allosaurus",
        "Chasmosaurus",    "Velociraptor", "Spinosaurus", "Ceratosaurus", "T-Rex"
    };
    static const int kDinoScore[9] = { 10, 15, 20, 30, 50, 100, 250, 300, 500 };

    // ── VIEW/INFO panel ───────────────────────────────────────────────────
    int viewPanel = 0, viewIdx = 0;
    TPicture previewPic = {};
    std::string previewText;

    auto loadPreview = [&]() {
        FreePic(previewPic); previewText.clear();
        switch (viewPanel) {
        case 0: if (viewIdx < kNumAreas) { SafeLoadTGA(previewPic, kAreaPics[viewIdx]); previewText = ReadTextFile(kAreaTxt[viewIdx],   512); } break;
        case 1: if (viewIdx < 9)         { SafeLoadTGA(previewPic, kDinoPics[viewIdx]); previewText = ReadTextFile(kDinoTxtM[viewIdx],  256); } break;
        case 2: if (viewIdx < 6)         { SafeLoadTGA(previewPic, kWeaponPics[viewIdx]); previewText = ReadTextFile(kWeaponTxt[viewIdx], 256); } break;
        case 3: if (viewIdx < 4)         { SafeLoadTGA(previewPic, kEquipPics[viewIdx]); previewText = ReadTextFile(kEquipNFO[viewIdx],  256); } break;
        }
    };
    loadPreview();

    // ── Right-align a number within a panel ───────────────────────────────
    // fnt_Small average char width ≈ 6px in screen coords
    auto drawRight = [&](int val, int pLeft, int pRight, int iy, uint32_t col) {
        char nb[12]; wsprintf(nb, "%d", val);
        int nx = pRight - (int)strlen(nb) * 6 - 70;
        MT(nb, nx, iy, col);
    };

    bool decided = false, confirmed = false;

    while (!decided && !appQuit) {
        if (!PollMenuEvents(appQuit)) break;

        // Compute cost before drawing so affordability is correct this frame
        int totalCost = kAreaCost[curArea];
        for (int d = 0; d < 9; d++)
            if (TargetDino & (1 << (d + AI_PARA))) totalCost += kDinoScore[d];
        for (int w = 0; w < 6; w++)
            if (WeaponPres & (1 << w)) totalCost += kWepCost[w];
        int remaining = TrophyRoom.Score - totalCost;

        // Build always-ON IDs: selected day tab + tranq/observer if active
        std::vector<int> alwaysOnIds;
        alwaysOnIds.push_back(curDay + 1);   // 1=Dawn, 2=Day, 3=Night
        if (tranqOn)  alwaysOnIds.push_back(5);
        if (observOn) alwaysOnIds.push_back(6);
        int hov = CompositeMenu(ms, alwaysOnIds);
        MenuBegin();
        DrawMenuScreen(ms);

        // ── Layout constants (800×600 design space scaled to WinW×WinH) ───
        int topH   = WinH * 288 / 600;   // VIEW/INFO section height
        int tabSY  = topH;
        int tabSH  = WinH * 37  / 600;   // tab bar height
        int listSY = tabSY + tabSH;
        int hdrH   = WinH * 48  / 600;   // panel header art height (icon + label)
        int listSH = WinH * 235 / 600;   // list area total height
        int panelW = WinW / 4;           // each of the 4 panels
        int lineH  = WinH * 16  / 600;   // list row height

        // ── TOP: preview image in VIEW frame ──────────────────────────────
        // Frame baked into MENU2 background at ≈ x=100-295, y=70-285 (800×600)
        {
            int fX = WinW * 100 / 800, fY = WinH *  70 / 600;
            int fW = WinW * 195 / 800, fH = WinH * 215 / 600;
            if (previewPic.lpImage && previewPic.W > 0 && previewPic.H > 0) {
                int pw = fW, ph = fH;
                int phFromW = pw * previewPic.H / previewPic.W;
                if (phFromW <= ph) ph = phFromW;
                else               pw = ph * previewPic.W / previewPic.H;
                OverlayPic(previewPic, fX + (fW - pw) / 2, fY + (fH - ph) / 2, pw, ph);
            }

            // Account balance and hunt cost in ACCOUNT section (centre-top background art)
            // ≈ x=355 (balance) and x=440 (cost) in 800-space
            int ay = WinH * 42 / 600;
            char buf[16];
            int displayRemaining = remaining < 0 ? 0 : remaining;
            wsprintf(buf, "%d", TrophyRoom.Score);
            MTMed(buf, WinW * 351 / 800, ay, 0x00AC6D24);
            wsprintf(buf, "%d", displayRemaining);
            MTMed(buf, WinW * 431 / 800, ay, 0x00AC6D24);

            // Info text in right section (≈ x=425, y=90 in 800×600)
            DrawMultiline(previewText.c_str(),
                          WinW * 425 / 800, WinH * 90 / 600,
                          WinH * 16 / 600, 0x00A0C0A0);
        }

        // ── TABS: click handling (selected state shown via ON graphic via alwaysOnIds) ─
        {
            for (int d = 0; d < 3; d++) {
                if (hov == d + 1 && gMI.lClick) curDay = d;
            }
            if (hov == 5 && gMI.lClick) tranqOn  = !tranqOn;
            if (hov == 6 && gMI.lClick) observOn = !observOn;
        }

        // ── FOUR LIST PANELS ───────────────────────────────────────────────
        // Panel headers (icon + LOCATIONS / DINOSAURS / WEAPONS / EQUIPMENT text)
        // are baked into the background art — we only draw list item rows.
        for (int p = 0; p < 4; p++) {
            int px  = p * panelW;
            int pr  = px + panelW;      // panel right edge
            int iy0 = listSY + hdrH;    // first list row y (below the header art)

            switch (p) {
            // ── LOCATIONS ─────────────────────────────────────────────────
            case 0: {
                for (int i = 0; i < kNumAreas; i++) {
                    int iy = iy0 + i * lineH;
                    if (iy + lineH > listSY + listSH) break;
                    bool sel       = (curArea == i);
                    int  delta     = kAreaCost[i] - kAreaCost[curArea];
                    bool canAfford = sel || (delta <= remaining);
                    bool hot = canAfford &&
                               gMI.x >= px && gMI.x < pr &&
                               gMI.y >= iy + 20 && gMI.y < iy + 20 + lineH;
                    uint32_t col;
                    if (!canAfford) col = 0x00505050;       // grey — can't afford
                    else if (sel)   col = 0x00FFD040;       // yellow — selected
                    else            col = 0x0040C8B0;       // cyan — affordable
                    MT(kAreaNames[i], px + 50, iy + 20, col);
                    drawRight(kAreaCost[i], px, pr, iy + 20, col);
                    if (hot) {
                        if (viewPanel != 0 || viewIdx != i) { viewPanel = 0; viewIdx = i; loadPreview(); }
                        if (gMI.lClick) curArea = i;
                    }
                }
                break;
            }
            // ── DINOSAURS (all 9 species hardcoded; free to toggle; at least 1 must stay) ──
            case 1: {
                for (int i = 0; i < 9; i++) {
                    int iy  = iy0 + i * lineH;
                    if (iy + lineH > listSY + listSH) break;
                    bool en  = (TargetDino & (1 << (i + AI_PARA))) != 0;
                    bool canAfford = en || (kDinoScore[i] <= remaining);
                    bool hot = canAfford &&
                               gMI.x >= px && gMI.x < pr &&
                               gMI.y >= iy + 20 && gMI.y < iy + 20 + lineH;
                    uint32_t col;
                    if (!canAfford) col = 0x00505050;
                    else if (en)    col = 0x00FFD040;
                    else            col = 0x0040C8B0;
                    MT(kDinoSpecies[i], px + 35, iy + 20, col);
                    drawRight(kDinoScore[i], px, pr, iy + 20, col);
                    if (hot) {
                        if (viewPanel != 1 || viewIdx != i) { viewPanel = 1; viewIdx = i; loadPreview(); }
                        if (gMI.lClick) TargetDino ^= (1 << (i + AI_PARA));
                    }
                }
                break;
            }
            // ── WEAPONS (all 6 hardcoded; cost enforced; at least 1 must stay) ──
            case 2: {
                for (int i = 0; i < 6; i++) {
                    int iy = iy0 + i * lineH;
                    if (iy + lineH > listSY + listSH) break;
                    bool sel       = (WeaponPres & (1 << i)) != 0;
                    bool canAfford = sel || (kWepCost[i] <= remaining);
                    bool hot = canAfford &&
                               gMI.x >= px && gMI.x < pr &&
                               gMI.y >= iy + 20 && gMI.y < iy + 20 + lineH;
                    uint32_t col;
                    if (!canAfford) col = 0x00505050;       // grey — can't afford
                    else if (sel)   col = 0x00FFD040;       // yellow — selected
                    else            col = 0x0040C8B0;       // cyan — affordable
                    MT(kWepNames[i], px + 35, iy + 20, col);
                    drawRight(kWepCost[i], px, pr, iy + 20, col);
                    if (hot) {
                        if (viewPanel != 2 || viewIdx != i) { viewPanel = 2; viewIdx = i; loadPreview(); }
                        if (gMI.lClick) WeaponPres ^= (1 << i);
                    }
                }
                break;
            }
            // ── EQUIPMENT (free; yellow=equipped, cyan=available) ─────────
            case 3: {
                for (int i = 0; i < 4; i++) {
                    int iy = iy0 + i * lineH;
                    if (iy + lineH > listSY + listSH) break;
                    bool en  = *equipFlags[i] != FALSE;
                    bool hot = gMI.x >= px && gMI.x < pr &&
                               gMI.y >= iy + 20 && gMI.y < iy + 20 + lineH;
                    // All equipment always selectable: yellow if equipped, cyan if not
                    uint32_t col = en ? 0x00FFD040 : 0x0040C8B0;
                    MT(kEquipNms[i], px + 35, iy + 20, col);
                    if (hot) {
                        if (viewPanel != 3 || viewIdx != i) { viewPanel = 3; viewIdx = i; loadPreview(); }
                        if (gMI.lClick) *equipFlags[i] = *equipFlags[i] ? FALSE : TRUE;
                    }
                }
                break;
            }
            }
        }

        // ── BOTTOM: BACK / HUNT (labels from background art via map IDs) ──
        if (gMI.lClick) {
            if (hov == 7) { decided = true; confirmed = false; }
            if (hov == 8 && totalCost <= TrophyRoom.Score) { decided = true; confirmed = true; }
        }
        if (gMI.scancode == SDL_SCANCODE_ESCAPE) { decided = true; confirmed = false; }

        MenuEnd();
        SDL_Delay(16);
    }

    FreePic(previewPic);
    FreeMenuScreen(ms);

    // Persist settings for next visit (whether Hunt or Back was pressed)
    s_curArea  = curArea;
    s_curDay   = curDay;
    s_tranqOn  = tranqOn;
    s_observOn = observOn;
    // TargetDino, WeaponPres, CamoMode, RadarMode, ScentMode, DoubleAmmo are globals — already persisted

    if (confirmed) {
        OptDayNight = curDay;
        Tranq       = tranqOn  ? TRUE : FALSE;
        ObservMode  = observOn ? TRUE : FALSE;
        if (curArea < kNumAreas)
            strcpy(ProjectName, kAreaFiles[curArea]);
    }
    return confirmed;
}

// ─── Credits ──────────────────────────────────────────────────────────────────

static void RunCredits(bool& appQuit) {
    MenuScreen ms = {};
    LoadMenuScreen(ms, "HUNTDAT\\MENU\\CREDITS.TGA", nullptr, nullptr);

    while (!appQuit) {
        if (!PollMenuEvents(appQuit)) break;
        if (gMI.lClick || gMI.scancode == SDL_SCANCODE_ESCAPE ||
            gMI.scancode == SDL_SCANCODE_RETURN) break;

        CompositeMenu(ms);
        MenuBegin();
        DrawMenuScreen(ms);
        MenuEnd();
        SDL_Delay(16);
    }
    FreeMenuScreen(ms);
}

// ─── Top-level entry point ────────────────────────────────────────────────────

bool RunMenus(bool& appQuit, bool skipToHunt, bool skipPlayerSelect) {
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_ShowCursor(SDL_ENABLE);

    // SOURCEPORT: Reset ambient-active tracking so MENUAMB restarts correctly after
    // returning from a hunt (AudioStop was called, clearing the ambient slot).
    gMenuAmbActive = nullptr;

    // Play ship-hum during player-select (MENUR) — the only screen that uses it.
    // MENUAMB takes over once the player reaches MENUM/MENU2/OPT.
    if (ShipModel.SoundFX[0].lpData)
        SetAmbient3d(ShipModel.SoundFX[0].length, ShipModel.SoundFX[0].lpData, 0.f, 0.f, 0.f);

    // When returning from a hunt: open hunt setup (MENU2) first.
    // If the player presses Back there, land on the main menu (MENUM), not player select.
    bool directToMainMenu = false;
    if (skipToHunt) {
        bool ready = RunHuntSetup(appQuit);
        if (appQuit) return false;
        if (ready) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
            SDL_ShowCursor(SDL_DISABLE);
            return true;
        }
        // Back pressed from MENU2 — skip player select, go straight to main menu
        directToMainMenu = true;
    }

    bool havePlayer = directToMainMenu || skipPlayerSelect;  // already have a player when returning from hunt or trophy
    while (!appQuit) {
        if (!havePlayer) {
            int slot = RunPlayerSelect(appQuit);
            if (appQuit || slot < 0) return false;

            if (gMenuNewPlayer) {
                gMenuNewPlayer = false;
                bool accepted = RunWaiver(appQuit);
                if (appQuit) return false;
                if (!accepted) continue;  // declined — back to player select
            }
            havePlayer = true;
        }

        bool exitToPlayerSelect = false;
        while (!exitToPlayerSelect && !appQuit) {
            int choice = RunMainMenu(appQuit);
            if (appQuit) return false;

            switch (choice) {
            case 0: {  // Hunt
                bool ready = RunHuntSetup(appQuit);
                if (appQuit) return false;
                if (ready) {
                    SDL_SetRelativeMouseMode(SDL_TRUE);
                    SDL_ShowCursor(SDL_DISABLE);
                    return true;
                }
                break;
            }
            case 1: {  // Trophy — launch TROPHY.MAP as a real level
                // SOURCEPORT: trophy room is a live 3D level, not a static menu screen.
                // TrophyMode is set by LoadResources when ProjectName contains "trophy".
                strcpy(ProjectName, "HUNTDAT/AREAS/trophy");
                SDL_SetRelativeMouseMode(SDL_TRUE);
                SDL_ShowCursor(SDL_DISABLE);
                return true;
            }
            case 2: RunOptions(appQuit);    break;   // Options
            case 3: RunCredits(appQuit);    break;   // Credits
            case 4: {  // Quit
                bool quit = RunQuitConfirm(appQuit);
                if (appQuit || quit) return false;
                break;
            }
            case 5:  // Change player (ESC on main menu)
                exitToPlayerSelect = true;
                havePlayer = false;
                break;
            default:
                return false;
            }
        }
    }
    return false;
}
