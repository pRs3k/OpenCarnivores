// SOURCEPORT: Menu system for OpenCarnivores — uses original Carnivores 2 artwork.
// Each menu screen is an 800x600 TGA pair (OFF/ON) plus a 400x300 RAW hit-test map.
// The hit-test map encodes button regions: each pixel byte is a button ID (0=background).
// On hover: pixels where map==hoveredId are taken from the ON image, rest from OFF image.
// This faithfully reproduces the original D3D menu visual style.

#include "Hunt.h"
#include <SDL.h>
#include "renderer/RendererGL.h"
#include "XR.h"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include "VFS.h"
#include "Bindings.h"
#include "Gamepad.h"
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
    uint32_t gen = 0;  // SOURCEPORT: generation counter; CompositeMenu uses to detect screen change
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
    int  padDX = 0;  // controller move direction set by PollMenuEvents (-1/0/+1)
    int  padDY = 0;
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

    // D-pad held-state for autorepeat — statics shared between event loop and post-loop.
    static Uint32 s_dpadRepeatAt = 0;
    static bool   s_dpadHeld     = false;
    static int    s_dpadDX = 0, s_dpadDY = 0;

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
        // SOURCEPORT: controller navigation — A = confirm, B/Start = back,
        // D-pad/stick = cycle items. NOT forwarded to Gamepad::HandleEvent
        // to avoid hunt-loop side-effects (weapon swap, pause, etc.) in menus.
        // D-pad fires on CONTROLLERBUTTONDOWN (not state-poll) so quick taps
        // that complete within a single frame are never silently dropped.
        case SDL_CONTROLLERBUTTONDOWN: {
            const uint8_t btn = ev.cbutton.button;
            if (btn == SDL_CONTROLLER_BUTTON_A) {
                gMI.lClick = true;
            } else if (btn == SDL_CONTROLLER_BUTTON_B || btn == SDL_CONTROLLER_BUTTON_START) {
                gMI.scancode = SDL_SCANCODE_ESCAPE;
            } else {
                int ddx = 0, ddy = 0;
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT)  ddx = -1;
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ddx =  1;
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_UP)    ddy = -1;
                if (btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)  ddy =  1;
                if (ddx || ddy) {
                    gMI.padDX = ddx; gMI.padDY = ddy;
                    s_dpadDX = ddx;  s_dpadDY = ddy;
                    s_dpadRepeatAt = SDL_GetTicks() + 400;
                    s_dpadHeld = true;
                }
            }
            break;
        }
        case SDL_CONTROLLERBUTTONUP: {
            const uint8_t btn = ev.cbutton.button;
            if (btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT  || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT ||
                btn == SDL_CONTROLLER_BUTTON_DPAD_UP    || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                s_dpadHeld = false;
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
            Gamepad::HandleEvent(ev);
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
        default:
            break;
        }
    }
    // SOURCEPORT: D-pad autorepeat + left-stick navigation.
    // D-pad initial fire happened in the CONTROLLERBUTTONDOWN case above.
    // Here we add repeat for held D-pad and immediate+repeat for the left stick.
    {
        SDL_GameController* pad = Gamepad::GetPad();
        if (pad) {
            Uint32 now = SDL_GetTicks();

            // D-pad repeat — fires after initial delay set in CONTROLLERBUTTONDOWN
            if (s_dpadHeld && now >= s_dpadRepeatAt) {
                gMI.padDX = s_dpadDX; gMI.padDY = s_dpadDY;
                s_dpadRepeatAt = now + 150;
            }

            // Left stick — only fires if D-pad didn't produce input this frame
            if (!gMI.padDX && !gMI.padDY) {
                static Uint32 s_stickRepeatAt = 0;
                static bool   s_stickHeld     = false;

                const int DEAD = 8000;
                int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
                int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
                int sdx = 0, sdy = 0;
                if      (lx < -DEAD) sdx = -1;
                else if (lx >  DEAD) sdx =  1;
                if (!sdx) {
                    if      (ly < -DEAD) sdy = -1;
                    else if (ly >  DEAD) sdy =  1;
                }
                if (sdx || sdy) {
                    if (!s_stickHeld) {
                        gMI.padDX = sdx; gMI.padDY = sdy;
                        s_stickRepeatAt = now + 400;
                        s_stickHeld = true;
                    } else if (now >= s_stickRepeatAt) {
                        gMI.padDX = sdx; gMI.padDY = sdy;
                        s_stickRepeatAt = now + 150;
                    }
                } else {
                    s_stickHeld = false;
                }
            }
        }
    }

    // Audio tick happens in menus too — crossfades between MENUR ship-hum and
    // MENUAMB depend on this firing every menu frame.
    AudioUpdate();
    return true;
}

// ─── Safe TGA loader (returns false instead of DoHalt on missing files) ───────

static bool SafeLoadTGA(TPicture& pic, const char* path) {
    // SOURCEPORT: resolve through the mod VFS so modded menu TGAs win.
    std::string resolved = VFS::ResolveRead(path);
    HANDLE hf = CreateFileA(resolved.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;
    CloseHandle(hf);
    LoadPictureTGA(pic, (LPSTR)resolved.c_str());
    conv_pic(pic);   // RGB555 → RGB565 for GL
    return true;
}

static void FreePic(TPicture& pic) {
    if (pic.lpImage) { HeapFree_(Heap, 0, pic.lpImage); pic.lpImage = nullptr; }
    pic.W = pic.H = 0;
}

// Load a MenuScreen: OFF tga, ON tga (optional), RAW map (optional).
static uint32_t s_menuGenCounter = 0;

static bool LoadMenuScreen(MenuScreen& ms, const char* offPath,
                           const char* onPath = nullptr,
                           const char* mapPath = nullptr)
{
    if (ms.loaded) return true;

    if (!SafeLoadTGA(ms.off, offPath)) return false;

    // Allocate composite buffer same size as OFF image
    ms.comp.W = ms.off.W; ms.comp.H = ms.off.H;
    ms.comp.lpImage = (WORD*)HeapAlloc_(Heap, 0, ms.comp.W * ms.comp.H * 2);
    memcpy(ms.comp.lpImage, ms.off.lpImage, ms.comp.W * ms.comp.H * 2);

    if (onPath) SafeLoadTGA(ms.on, onPath);

    if (mapPath) {
        // SOURCEPORT: route through VFS so modded .RAW hit-test maps override retail.
        FILE* f = VFS::fopen(mapPath, "rb");
        if (f) {
            ms.map.resize(ms.mapW * ms.mapH, 0);
            fread(ms.map.data(), 1, ms.mapW * ms.mapH, f);
            fclose(f);
        }
    }

    ms.loaded = true;
    ms.gen = ++s_menuGenCounter;
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

    // ── Controller focus navigation ───────────────────────────────────────────
    // Centroid table: button ID → {cx, cy} in 400×300 map space.
    // Rebuilt whenever the active menu screen changes.
    // On D-pad/stick input (padDX/padDY), the cursor warps to the nearest button
    // centroid in the pressed direction so the normal hit-test below picks it up.
    static int      s_focusId = 0;
    static uint32_t s_lastGen = 0;  // SOURCEPORT: gen counter immune to heap/stack address reuse
    static std::map<int, std::pair<int,int>> s_centroids;

    if (ms.gen != s_lastGen) {
        s_lastGen = ms.gen;
        s_centroids.clear();
        s_focusId = 0;
        if (!ms.map.empty()) {
            std::map<int,int> cnt, sx, sy;
            for (int iy = 0; iy < ms.mapH; iy++) {
                for (int ix = 0; ix < ms.mapW; ix++) {
                    uint8_t id = ms.map[iy * ms.mapW + ix];
                    if (id > 0 && id != 42) { cnt[id]++; sx[id]+=ix; sy[id]+=iy; }
                }
            }
            for (auto& [id, n] : cnt)
                s_centroids[id] = { sx[id]/n, sy[id]/n };
            // Default focus: topmost button (smallest map-Y centroid)
            int topY = ms.mapH, topId = 0;
            for (auto& [id, c] : s_centroids)
                if (c.second < topY) { topY = c.second; topId = id; }
            s_focusId = topId;
        }
    }

    if ((gMI.padDX || gMI.padDY) && !s_centroids.empty()) {
        // Snap cursor to current focus centroid, then find the nearest button in
        // the pressed direction via dot-product score (penalise lateral offset).
        auto cur = s_centroids.count(s_focusId)
            ? s_centroids.at(s_focusId)
            : std::make_pair(ms.mapW / 2, ms.mapH / 2);
        if (s_centroids.count(s_focusId)) {
            gMI.x = cur.first  * WinW / ms.mapW;
            gMI.y = cur.second * WinH / ms.mapH;
        }
        int   bestId    = 0;
        float bestScore = 1e30f;
        for (auto& [id, c] : s_centroids) {
            if (id == s_focusId) continue;
            float relX  = (float)(c.first  - cur.first);
            float relY  = (float)(c.second - cur.second);
            float dot   = relX * gMI.padDX + relY * gMI.padDY;
            if (dot <= 0.f) continue;
            float score = dot + std::abs(relX * gMI.padDY - relY * gMI.padDX) * 2.f;
            if (score < bestScore) { bestScore = score; bestId = id; }
        }
        if (bestId) {
            s_focusId = bestId;
            gMI.x = s_centroids[bestId].first  * WinW / ms.mapW;
            gMI.y = s_centroids[bestId].second * WinH / ms.mapH;
        }
        gMI.padDX = gMI.padDY = 0;
    }

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

    // Keep focus in sync with mouse so switching input feels natural
    if (hoverId > 0) s_focusId = hoverId;

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
    // SOURCEPORT: pass &pic as overrideKey so a registered PNG/DDS sibling wins.
    // lpImage is heap-recycled across ReleaseResources; &pic is stable for the
    // lifetime of this TPicture.
    g_glRenderer->DrawBitmap(x, y, w, h, pic.W, pic.lpImage, false, pic.H, &pic);
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

// SOURCEPORT: big heading text — used for section headings like the "MODS" screen title.
static void MTBig(const char* s, int x, int y, uint32_t col = 0x00FFFFFF) {
    if (s && s[0]) {
        int sh = std::max(2, WinH / 360);
        g_glRenderer->DrawTextBig(x+sh, y+sh, s, 0x00000000);
        g_glRenderer->DrawTextBig(x,    y,    s, col);
    }
}

// Read a text file up to maxBytes.
static std::string ReadTextFile(const char* path, int maxBytes = 512) {
    std::string out;
    // SOURCEPORT: area/description .TXT files live under HUNTDAT — route via VFS.
    FILE* f = VFS::fopen(path, "r");
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

// Tracks whether we're at the very start of a menu session so we can
// reposition the world-locked quad in front of the player on entry.
static bool s_menuFirstFrame = true;

// Controller cursor position (screen/drawable coords) updated each frame in
// MenuBegin() when the controller ray hits the menu quad.
static bool  s_ctrlCursorValid = false;
static float s_ctrlCursorX     = 0.f;
static float s_ctrlCursorY     = 0.f;

static void MenuBegin() {
    // SOURCEPORT: keep the XR compositor alive during menus.
    XR::PollEvents();
    XR::BeginFrame();
    g_glRenderer->BeginFrame();
    g_glRenderer->ClearBuffers();

    // Re-anchor the quad pose on the first frame of a new menu session.
    if (s_menuFirstFrame && XR::StereoActive()) {
        XR::ResetMenuQuadPose();
        s_menuFirstFrame = false;
    }

    // One-time diagnostic: log action-system state when VR is first active.
    {
        static bool s_diagDone = false;
        if (!s_diagDone && XR::StereoActive()) {
            s_diagDone = true;
            PrintLog(XR::ActionsReady()
                ? (char*)"[VR] Action system ready — controller aim should work\n"
                : (char*)"[VR] Action system NOT ready — using head-gaze only\n");
        }
    }

    // SOURCEPORT: drive the menu cursor from VR input when in stereo mode.
    // Priority: (1) controller aim ray (OpenXR action system), falling back to
    // (2) head-gaze ray (uses already-working view poses — always available).
    // Click sources: (a) XR trigger via XInput, (b) SDL gamepad trigger/A.
    s_ctrlCursorValid = false;
    if (XR::StereoActive()) {
        float sx = 0.f, sy = 0.f;
        bool  havePos      = false;
        bool  justPressed  = false;
        bool  justReleased = false;

        // ── Try controller aim ────────────────────────────────────────────────
        for (int hand = 0; hand < 2; ++hand) {
            bool pressed, jp, jr;
            if (XR::GetControllerMenuCursor(hand, sx, sy, pressed, jp, jr)) {
                havePos      = true;
                justPressed  = jp;
                justReleased = jr;
                break;
            }
        }

        // ── Fallback: head-gaze ───────────────────────────────────────────────
        if (!havePos)
            havePos = XR::GetHeadGazeCursor(sx, sy);

        if (havePos) {
            s_ctrlCursorValid = true;
            s_ctrlCursorX     = sx;
            s_ctrlCursorY     = sy;
            gMI.x = (int)sx;
            gMI.y = (int)sy;
            if (justPressed)  { gMI.lClick = true; gMI.lHeld = true;  }
            if (justReleased) {                     gMI.lHeld = false; }
        }

        // ── Click detection: XInput → SDL gamepad → XR trigger ───────────────
        // XInput is polled directly (no SDL, no window focus required) so Quest
        // controllers via PC Link are always seen even when the SDL window is
        // unfocused.  SDL gamepad and XR trigger are checked as fallbacks.
        {
            // Lazy-load XInput so the exe still runs without a VR runtime.
            static bool                     s_xiLoaded = false;
            static HMODULE                  s_xiDll    = nullptr;
            typedef DWORD (WINAPI *PFN_XI)(DWORD, void*);
            static PFN_XI                   s_xiGet    = nullptr;
            if (!s_xiLoaded) {
                s_xiLoaded = true;
                s_xiDll = LoadLibraryA("xinput1_4.dll");
                if (!s_xiDll) s_xiDll = LoadLibraryA("xinput9_1_0.dll");
                if (s_xiDll) s_xiGet = (PFN_XI)GetProcAddress(s_xiDll, "XInputGetState");
                PrintLog(s_xiGet ? (char*)"[VR] XInput loaded OK\n"
                                 : (char*)"[VR] XInput not available\n");
            }

            // XINPUT_GAMEPAD layout (verified against xinput.h):
            //   DWORD  dwPacketNumber
            //   WORD   wButtons        — 0x1000=A, 0x2000=B, 0x4000=X, 0x8000=Y
            //   BYTE   bLeftTrigger    — 0-255
            //   BYTE   bRightTrigger   — 0-255
            //   SHORT  sThumbLX/LY/RX/RY
            struct XiState {
                DWORD  packet;
                WORD   buttons;
                BYTE   ltrig, rtrig;
                SHORT  lx, ly, rx, ry;
            };

            static bool s_clickPrev = false;
            bool click = false;

            // Try all 4 XInput slots (Quest may appear on any slot).
            if (s_xiGet) {
                for (DWORD i = 0; i < 4 && !click; ++i) {
                    XiState st = {};
                    if (s_xiGet(i, &st) == 0 /* ERROR_SUCCESS */) {
                        click = (st.ltrig  > 100)
                             || (st.rtrig  > 100)
                             || (st.buttons & 0x1000)  // A
                             || (st.buttons & 0x2000)  // B
                             || (st.buttons & 0x4000)  // X
                             || (st.buttons & 0x8000); // Y
                    }
                }
            }

            // SDL gamepad fallback (works if SDL already opened the controller).
            if (!click) {
                SDL_GameController* pad = Gamepad::GetPad();
                if (pad) {
                    click = (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > 16000)
                         || (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
                         || (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) != 0);
                }
            }

            if (click  && !s_clickPrev) { gMI.lClick = true; gMI.lHeld = true;  }
            if (!click &&  s_clickPrev) {                     gMI.lHeld = false; }
            s_clickPrev = click;
        }
    }
}
static void MenuEnd() {
    // SOURCEPORT: blit the menu into a world-locked XrCompositionLayerQuad.
    // The quad is placed in front of wherever the player was looking when the
    // menu opened, and stays fixed even as the player looks around.
    if (XR::StereoActive()) {
        unsigned int menuFbo = XR::AcquireMenuImage();
        if (menuFbo) {
            int srcW = WinW, srcH = WinH;
            int dstW = (int)XR::MenuImageWidth(), dstH = (int)XR::MenuImageHeight();
            // Scale to fit width; black bars top/bottom if aspect ratios differ.
            int blitH = srcH * dstW / srcW;
            int blitY0 = (dstH - blitH) / 2;

            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)menuFbo);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)menuFbo);
            glBlitFramebuffer(0, 0, srcW, srcH,
                              0, blitY0, dstW, blitY0 + blitH,
                              GL_COLOR_BUFFER_BIT, GL_LINEAR);

            // SOURCEPORT: Draw controller cursor as crosshair instead of circle.
            if (s_ctrlCursorValid) {
                int cx = (int)(s_ctrlCursorX * dstW / WinW);
                // s_ctrlCursorY is Y-down; OpenGL FBO origin is Y-up.
                int cy_down = blitY0 + (int)(s_ctrlCursorY * blitH / WinH);
                int cy = dstH - cy_down;  // flip to GL Y-up

                glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)menuFbo);
                glEnable(GL_SCISSOR_TEST);

                // Draw crosshair cursor (+ shape)
                const int sz = 8;  // size of crosshair
                const int th = 2;  // thickness

                // Horizontal bar - black
                glScissor(cx - sz, cy - th, sz * 2, th * 2);
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Vertical bar - black
                glScissor(cx - th, cy - sz, th * 2, sz * 2);
                glClear(GL_COLOR_BUFFER_BIT);

                // Center dot - white
                glScissor(cx - 1, cy - 1, 2, 2);
                glClearColor(1.f, 1.f, 1.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);

                glDisable(GL_SCISSOR_TEST);
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            XR::ReleaseMenuImage();
        }
    }
    g_glRenderer->EndFrame();
    XR::EndFrame();
}

// ─── Save file helpers ────────────────────────────────────────────────────────

struct PlayerSlot { bool exists; char name[128]; int score; int rank; };

static PlayerSlot ReadSlot(int n) {
    PlayerSlot ps = {};
    char fname[64]; wsprintf(fname, "trophy0%d.sav", n);
    HANDLE hf = CreateFileA(fname, GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return ps;
    TTrophyRoom tr = {}; DWORD l;
    ReadFile(hf, &tr, sizeof(tr), &l, nullptr);
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

    // SOURCEPORT: find OK (id=1) and DELETE (id=2) centroids for controller cursor warp
    int okRX = -1, okRY = -1, delRX = -1, delRY = -1;
    if (!ms.map.empty()) {
        long long sx1=0,sy1=0,n1=0,sx2=0,sy2=0,n2=0;
        for (int my=0;my<ms.mapH;my++) for (int mx=0;mx<ms.mapW;mx++) {
            uint8_t id=ms.map[my*ms.mapW+mx];
            if(id==1){sx1+=mx;sy1+=my;n1++;}
            if(id==2){sx2+=mx;sy2+=my;n2++;}
        }
        if(n1){okRX=(int)(sx1/n1)*WinW/ms.mapW;okRY=(int)(sy1/n1)*WinH/ms.mapH;}
        if(n2){delRX=(int)(sx2/n2)*WinW/ms.mapW;delRY=(int)(sy2/n2)*WinH/ms.mapH;}
    }
    int focusIdx = 0;  // 0-4=player slots, 5=OK, 6=DELETE
    // SOURCEPORT: put cursor in neutral position so no map button is pre-highlighted on entry
    gMI.x = lx + WinW*90/800;
    gMI.y = ly + slH/2;  // center of slot 0 (not in map button area)

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

        // SOURCEPORT: controller navigation for player slots and OK/DELETE
        for (int i = 0; i < 5; i++) {
            int sy = ly + i * slH;
            if (gMI.x >= lx && gMI.x < lx + WinW*180/800 &&
                gMI.y >= sy  && gMI.y < sy + slH) focusIdx = i;
        }
        {
            int prev = focusIdx;
            if (gMI.padDY > 0) { if (focusIdx < 6) ++focusIdx; gMI.padDX=0; gMI.padDY=0; }
            if (gMI.padDY < 0) { if (focusIdx > 0) --focusIdx; gMI.padDX=0; gMI.padDY=0; }
            if (focusIdx != prev && focusIdx >= 0 && focusIdx <= 4) {
                PlayerSlot ps = ReadSlot(focusIdx);
                if (ps.exists) { highlightSlot=focusIdx; strncpy(typedName,ps.name,31); typedName[31]=0; }
                else highlightSlot=-1;
                // Warp cursor into slot area so map buttons (OK/DELETE) don't stay highlighted
                gMI.x = lx + WinW*90/800;
                gMI.y = ly + focusIdx * slH + slH/2;
            }
        }
        if (focusIdx == 5 && okRX  >= 0) { gMI.x = okRX;  gMI.y = okRY; }
        if (focusIdx == 6 && delRX >= 0) { gMI.x = delRX; gMI.y = delRY; }

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
            bool hot = (gMI.x >= lx && gMI.x < lx + WinW*180/800 &&
                        gMI.y >= sy  && gMI.y < sy + slH) || (focusIdx == i);
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

        // SOURCEPORT: controller A on focused slot — first press selects, second confirms (= Return)
        if (gMI.lClick && hov == 0 && focusIdx >= 0 && focusIdx <= 4) {
            PlayerSlot fps = ReadSlot(focusIdx);
            if (fps.exists) {
                if (highlightSlot == focusIdx) sc = SDL_SCANCODE_RETURN;
                else { highlightSlot=focusIdx; strncpy(typedName,fps.name,31); typedName[31]=0; }
            }
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

        // SOURCEPORT: "Mods" link in the bottom-right corner — large heading text
        // so it's readable at a glance without baked button art. Hit rect matches
        // the rendered glyph bounds.
        const char* kModsLabel = "MODS";
        int mLabelW = g_glRenderer->MeasureTextBig(kModsLabel);
        int mLabelH = WinH * 44 / 600;
        int mLabelX = WinW - mLabelW - WinW * 20 / 800;
        int mLabelY = WinH - mLabelH - WinH * 12 / 600;
        int mHitX0  = mLabelX - WinW * 6 / 800;
        int mHitY0  = mLabelY - WinH * 4 / 600;
        int mHitX1  = mLabelX + mLabelW + WinW * 6 / 800;
        int mHitY1  = mLabelY + mLabelH;
        bool mHot = (gMI.x >= mHitX0 && gMI.x < mHitX1 &&
                     gMI.y >= mHitY0 && gMI.y < mHitY1);
        MTBig(kModsLabel, mLabelX, mLabelY, mHot ? 0x00FFE080 : 0x00AC6D24);

        if (gMI.lClick) {
            if (mHot) { result = 6; }                    // Mods — SOURCEPORT
            else switch (hov) {
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

    // Track which slider is currently being dragged (to prevent multiple sliders responding)
    static int dragSliderTx = -1, dragSliderTy = -1;

    // Draw horizontal slider using SL_BAR.TGA / SL_BUT.TGA; returns updated value on click.
    auto DrawSlider = [&](int tx, int ty, int tw, int th,
                          int val, int minVal, int maxVal) -> int {
        int range = maxVal - minVal;
        // Bar background
        if (slBar.lpImage && slBar.W > 0)
            g_glRenderer->DrawBitmap(tx, ty, tw, th, slBar.W, slBar.lpImage, false, slBar.H, &slBar);
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
                g_glRenderer->DrawBitmap(thumbX, thumbY, butW, butH, slBut.W, (void*)slBut.lpImage, true, slBut.H, &slBut);
            else
                g_glRenderer->FillRect(thumbX, thumbY, butW, butH + 4, 0xFFC09060);
        }
        // Handle slider dragging: track which slider is active to prevent multi-slider response
        if (!gMI.lHeld) {
            dragSliderTx = dragSliderTy = -1;  // Mouse released, clear drag state
        } else if (dragSliderTx == tx && dragSliderTy == ty) {
            // This slider is currently being dragged; continue drag with free movement
            int butH   = (slBut.H > 0) ? slBut.H * th / std::max(1, slBar.H) : th + 4;
            int butW   = (slBut.W > 0) ? slBut.W * butH / std::max(1, slBut.H) : 8;
            int travel = tw - butW;
            int clampedX = std::max(tx, std::min(gMI.x, tx + tw));
            int v = (travel > 0) ? minVal + (clampedX - tx) * range / travel : minVal;
            return std::max(minVal, std::min(v, maxVal));
        } else if (dragSliderTx == -1) {
            // No slider currently being dragged; check if this one is clicked on the button
            int butH   = (slBut.H > 0) ? slBut.H * th / std::max(1, slBar.H) : th + 4;
            int butW   = (slBut.W > 0) ? slBut.W * butH / std::max(1, slBut.H) : 8;
            int travel = tw - butW;
            int thumbX = tx + (travel > 0 ? (val - minVal) * travel / range : 0);

            // Check if cursor is on or near the button (with margin)
            int buttonMargin = 8;
            if (gMI.x >= thumbX - buttonMargin && gMI.x < thumbX + butW + buttonMargin &&
                gMI.y >= ty-4 && gMI.y < ty+th+4) {
                dragSliderTx = tx;
                dragSliderTy = ty;
                // Apply drag for this frame
                int clampedX = std::max(tx, std::min(gMI.x, tx + tw));
                int v = (travel > 0) ? minVal + (clampedX - tx) * range / travel : minVal;
                return std::max(minVal, std::min(v, maxVal));
            }
        }
        return val;
    };

    // Key rebinding state: waitIdx = index into bindings[] being rebound (-1 = none)
    // waitCol: 0=keyboard, 1=gamepad, 2=VR controller
    struct Binding { const char* name; int* vk; int* pad; int* vr; };
    Binding bindings[] = {
        { "Forward",     &KeyMap.fkForward,  &PadMap.fkForward,  &VRMap.fkForward  },
        { "Backward",    &KeyMap.fkBackward, &PadMap.fkBackward, &VRMap.fkBackward },
        { "Turn Up",     &KeyMap.fkUp,       &PadMap.fkUp,       &VRMap.fkUp       },
        { "Turn Down",   &KeyMap.fkDown,     &PadMap.fkDown,     &VRMap.fkDown     },
        { "Turn Left",   &KeyMap.fkLeft,     &PadMap.fkLeft,     &VRMap.fkLeft     },
        { "Turn Right",  &KeyMap.fkRight,    &PadMap.fkRight,    &VRMap.fkRight    },
        { "Fire",        &KeyMap.fkFire,     &PadMap.fkFire,     &VRMap.fkFire     },
        { "Get weapon",  &KeyMap.fkShow,     &PadMap.fkShow,     &VRMap.fkShow     },
        { "Step Left",   &KeyMap.fkSLeft,    &PadMap.fkSLeft,    &VRMap.fkSLeft    },
        { "Step Right",  &KeyMap.fkSRight,   &PadMap.fkSRight,   &VRMap.fkSRight   },
        { "Strafe",      &KeyMap.fkStrafe,   &PadMap.fkStrafe,   &VRMap.fkStrafe   },
        { "Jump",        &KeyMap.fkJump,     &PadMap.fkJump,     &VRMap.fkJump     },
        { "Run",         &KeyMap.fkRun,      &PadMap.fkRun,      &VRMap.fkRun      },
        { "Crouch",      &KeyMap.fkCrouch,   &PadMap.fkCrouch,   &VRMap.fkCrouch   },
        { "Call",        &KeyMap.fkCall,     &PadMap.fkCall,     &VRMap.fkCall     },
        { "Change Call", &KeyMap.fkCCall,    &PadMap.fkCCall,    &VRMap.fkCCall    },
        { "Binoculars",  &KeyMap.fkBinoc,    &PadMap.fkBinoc,    &VRMap.fkBinoc    },
    };
    const int kNumBindings = (int)(sizeof(bindings)/sizeof(bindings[0]));
    int  waitIdx = -1;   // index of binding waiting for input
    int  waitCol = 0;    // 0=key, 1=pad, 2=vr

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
        gMI.lClick = false; gMI.rClick = false; gMI.scancode = 0; gMI.padDX = 0; gMI.padDY = 0;
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:   appQuit = true; break;
            case SDL_MOUSEMOTION:   ScaleMouse(ev.motion.x, ev.motion.y); break;
            case SDL_MOUSEBUTTONDOWN:
                ScaleMouse(ev.button.x, ev.button.y);
                if (ev.button.button == SDL_BUTTON_LEFT)  { gMI.lClick = true; gMI.lHeld = true; }
                if (ev.button.button == SDL_BUTTON_RIGHT) gMI.rClick = true;
                // While waiting for a key (not pad/vr): mouse buttons count too.
                if (waitIdx >= 0 && waitCol == 0) {
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
                    // ESC clears the binding. Key slot zeroed; pad/vr set to -1.
                    if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        if      (waitCol == 1) *bindings[waitIdx].pad = -1;
                        else if (waitCol == 2) *bindings[waitIdx].vr  = -1;
                        else                   *bindings[waitIdx].vk  =  0;
                        waitIdx = -1;
                    } else if (waitCol == 0) {
                        int vk = ScancodeToVK(ev.key.keysym.scancode);
                        if (vk) { *bindings[waitIdx].vk = vk; waitIdx = -1; }
                    }
                    // Keyboard input while waiting on pad or VR column is ignored
                    // (except ESC) — user must press the appropriate device input.
                } else {
                    gMI.scancode = ev.key.keysym.scancode;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (waitIdx >= 0 && waitCol == 1) {
                    *bindings[waitIdx].pad = ev.cbutton.button;
                    waitIdx = -1;
                } else {
                    // SOURCEPORT: menu navigation when not rebinding
                    switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_A:          gMI.lClick = true; break;
                    case SDL_CONTROLLER_BUTTON_B:
                    case SDL_CONTROLLER_BUTTON_START:      gMI.scancode = SDL_SCANCODE_ESCAPE; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  gMI.padDX = -1; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: gMI.padDX = +1; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    gMI.padDY = -1; break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  gMI.padDY = +1; break;
                    }
                }
                break;
            // SOURCEPORT: same rationale as PollMenuEvents — forward hot-plug
            // so late-enumerating devices reach Gamepad::HandleEvent.
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                Gamepad::HandleEvent(ev);
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

        // Pad axis/button capture (waitCol==1).
        if (waitIdx >= 0 && waitCol == 1) {
            int vbtn = PollPadAxisEdge();
            if (!vbtn) {
                int btn = PollPadButtonEdge();
                if (btn) vbtn = btn + 1000;
            }
            if (vbtn) { *bindings[waitIdx].pad = vbtn; waitIdx = -1; }
        }
        // VR button capture (waitCol==2).
        if (waitIdx >= 0 && waitCol == 2) {
            int vrBtn = XR::PollVRBtnEdge();
            if (vrBtn) { *bindings[waitIdx].vr = vrBtn; waitIdx = -1; }
        }

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
                // SOURCEPORT: range kept at retail 1..255 for save-file
                // compatibility. ApplyViewRange() uses a non-linear curve so
                // slider mid (128) → ctViewR≈96 (slightly > retail 82), and
                // slider max (255) → ctViewR 250 (enlarged VMap[512][512] cap).
                { "View range",  &OptViewR, 1, 255 },
            };
            for (auto& s : sliders) {
                MTMed(s.lbl, ox, y, 0x00AC6D24);
                *s.var = DrawSlider(ox + lblW, y + 2, slW, lnH - 6, *s.var, s.mn, s.mx);
                // Display percentage (100% at midpoint 128)
                char buf[32];
                wsprintf(buf, "%d%%", (*s.var * 100) / 128);
                MT(buf, ox + lblW + slW + 8, y, 0x00C0C0C0);
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
            int y   = WinH * 335 / 600;
            int vx  = ox + lblW;
            int tvW = WinW * 130 / 800;  // clickable value column width

            // Audio / video driver (read-only)
            MTMed("Audio Driver", ox, y, 0x00AC6D24); MT("OpenAL Soft",  vx, y, 0x00C0C0C0); y += lnH;
            MTMed("Video Driver", ox, y, 0x00AC6D24); MT("OpenGL 4.1 Core",  vx, y, 0x00C0C0C0); y += lnH;

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
                // Display percentage (0 = 100%)
                char buf[32];
                wsprintf(buf, "%d%%", 100 + (OptBrightness * 100) / 128);
                MT(buf, vx + slW + 8, y, 0x00C0C0C0);
            }
            y += lnH;

            // Anisotropic filtering level
            {
                MTMed("Anisotropy", ox, y, 0x00AC6D24);
                int prev = OptAnisoLevel;
                OptAnisoLevel = DrawSlider(vx, y+2, slW, lnH-6, OptAnisoLevel, 1, 4);
                if (OptAnisoLevel != prev) {
                    // Applied immediately in RendererGL.cpp SetTexture()
                }
                // Display label with actual hardware max for level 4
                char buf[32];
                if (OptAnisoLevel < 4) {
                    static const char* anisoLabels[] = { "Low (2x)", "Medium (4x)", "High (8x)" };
                    MT(anisoLabels[OptAnisoLevel - 1], vx + slW + 8, y, 0x00C0C0C0);
                } else {
                    int maxAniso = g_glRenderer ? g_glRenderer->GetMaxAnisotropy() : 16;
                    wsprintf(buf, "Max (%dx)", maxAniso);
                    MT(buf, vx + slW + 8, y, 0x00C0C0C0);
                }
            }
            y += lnH;

            // Supersampling (VR eye FBO multiplier; ignored on flatscreen)
            {
                MTMed("Supersampling (VR Only)", ox, y, 0x00AC6D24);
                int prev = OptSSFactor;
                OptSSFactor = DrawSlider(vx, y+2, slW, lnH-6, OptSSFactor, 100, 200);
                if (OptSSFactor != prev) {
                    // SOURCEPORT: recreate swapchains with new supersampling scale
                    if (XR::SessionRunning()) {
                        if (!XR::ReconfigureSwapchainResolution()) {
                            OptSSFactor = prev;  // revert on failure
                        }
                    }
                }
                char buf[32];
                wsprintf(buf, "%d%%", OptSSFactor);
                MT(buf, vx + slW + 8, y, 0x00C0C0C0);
            }
        }

        // ── CONTROLS panel ─────────────────────────────────────────────────
        // Four columns: action name | keyboard | gamepad | VR controller.
        // Click any value column to rebind; ESC cancels an active rebind.
        {
            char kbuf[32];
            int ox   = WinW * 430 / 800;   // action label left edge
            int kx   = WinW * 515 / 800;   // keyboard column
            int px   = WinW * 598 / 800;   // gamepad column
            int vx   = WinW * 681 / 800;   // VR controller column
            int colW = WinW *  78 / 800;   // clickable width per column
            int y    = WinH * 80 / 600;
            int lnH2 = WinH * 24 / 600;

            for (int i = 0; i < kNumBindings; i++) {
                bool waitingKey = (waitIdx == i && waitCol == 0);
                bool waitingPad = (waitIdx == i && waitCol == 1);
                bool waitingVR  = (waitIdx == i && waitCol == 2);
                bool hotKey = waitIdx < 0 && gMI.x >= kx && gMI.x < kx+colW &&
                              gMI.y >= y && gMI.y < y+lnH2;
                bool hotPad = waitIdx < 0 && gMI.x >= px && gMI.x < px+colW &&
                              gMI.y >= y && gMI.y < y+lnH2;
                bool hotVR  = waitIdx < 0 && gMI.x >= vx && gMI.x < vx+colW &&
                              gMI.y >= y && gMI.y < y+lnH2;

                MTMed(bindings[i].name, ox, y,
                      (waitingKey || waitingPad || waitingVR) ? 0x00FFFFFF : 0x00AC6D24);

                // Keyboard column
                if (waitingKey) {
                    MT("Press key...", kx, y, 0x00FFFF00);
                } else {
                    VKStr(*bindings[i].vk, kbuf, sizeof(kbuf));
                    MT(kbuf, kx, y, hotKey ? 0x00FFFF40 : 0x00C0C0C0);
                    if (hotKey && gMI.lClick) { waitIdx = i; waitCol = 0; gMI.lClick = false; }
                }
                // Gamepad column
                if (waitingPad) {
                    MT("Press pad...", px, y, 0x00FFFF00);
                } else {
                    MT(PadBtnName(*bindings[i].pad), px, y, hotPad ? 0x00FFFF40 : 0x00C0C0C0);
                    if (hotPad && gMI.lClick) { waitIdx = i; waitCol = 1; gMI.lClick = false; }
                }
                // VR controller column
                if (waitingVR) {
                    MT("Press VR...", vx, y, 0x00FFFF00);
                } else {
                    MT(XR::VRBtnName(*bindings[i].vr), vx, y, hotVR ? 0x00FFFF40 : 0x00C0C0C0);
                    if (hotVR && gMI.lClick) { waitIdx = i; waitCol = 2; gMI.lClick = false; XR::PollVRBtnEdge(); }
                }
                y += lnH2;
                if (y > WinH * 530 / 600) break;
            }

            // Hint line + Reset button
            {
                MT("ESC: clear binding", ox, y, 0x00888888);
                int bx = vx, bw = colW;
                bool hotR = waitIdx < 0 && gMI.x >= bx && gMI.x < bx+bw &&
                            gMI.y >= y && gMI.y < y+lnH2;
                MT("Reset", bx, y, hotR ? 0x00FFFF40 : 0x00C0C0C0);
                if (hotR && gMI.lClick) {
                    Bindings::ResetToDefaults();
                    gMI.lClick = false;
                }
                y += lnH2;
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
            OptMsSens = DrawSlider(kx + 20, y - 14, WinW*130/800, lnH2-6, OptMsSens, 1, 20);
            // Display percentage (10 = 100%)
            char buf[32];
            wsprintf(buf, "%d%%", (OptMsSens * 100) / 10);
            MT(buf, kx + 20 + WinW*130/800 + 8, y - 14, 0x00C0C0C0);
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
    Bindings::Save();    // SOURCEPORT: persist key rebinds to controls.cfg
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
    // SOURCEPORT: preview assets are indexed in canonical game order
    // (CAMOFLAG, SCENT, RADAR, TRANQ); MENU2's display order is different and
    // swaps in Double Ammo for Tranq. Translate display row → preview assets.
    static const char* kEquipPicsM2[4] = {
        "HUNTDAT\\MENU\\PICS\\EQUIP1.TGA",  // Camouflage
        "HUNTDAT\\MENU\\PICS\\EQUIP3.TGA",  // Radar
        "HUNTDAT\\MENU\\PICS\\EQUIP2.TGA",  // Cover scent
        "HUNTDAT\\MENU\\PICS\\EQUIP4.TGA",  // Double Ammo (no dedicated TGA; reuse)
    };
    static const char* kEquipNFOM2[4] = {
        "HUNTDAT\\MENU\\TXT\\CAMOFLAG.NFO",
        "HUNTDAT\\MENU\\TXT\\RADAR.NFO",
        "HUNTDAT\\MENU\\TXT\\SCENT.NFO",
        "HUNTDAT\\MENU\\TXT\\DOUBLE.NFO",
    };

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
        case 3: if (viewIdx < 4)         { SafeLoadTGA(previewPic, kEquipPicsM2[viewIdx]); previewText = ReadTextFile(kEquipNFOM2[viewIdx],  256); } break;
        // SOURCEPORT: Tranq is not in the equipment column (it's a tab toggle);
        // give it its own preview case so hovering the tab shows the NFO.
        case 4: { SafeLoadTGA(previewPic, "HUNTDAT\\MENU\\PICS\\EQUIP4.TGA");
                  previewText = ReadTextFile("HUNTDAT\\MENU\\TXT\\TRANQ.NFO", 256); } break;
        }
    };
    loadPreview();

    // ── Right-align a number within a panel ───────────────────────────────
    // fnt_Small average char width ≈ 6px in screen coords
    auto drawRight = [&](int val, int /*pLeft*/, int pRight, int iy, uint32_t col) {
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
            if (hov == 5) {
                if (viewPanel != 4) { viewPanel = 4; viewIdx = 0; loadPreview(); }
                if (gMI.lClick) tranqOn = !tranqOn;
            }
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
            snprintf(ProjectName, sizeof(ProjectName), "%s", kAreaFiles[curArea]);
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

// ─── Mods ────────────────────────────────────────────────────────────────────
// SOURCEPORT: enumerates ./mods/* directories, lets the user toggle each one
// enabled/disabled, and persists the enabled list to mods.cfg (one folder name
// per line). The VFS that actually honours this list is a future feature — for
// now the screen is the UI landing; changes "apply on next launch" once the VFS
// is wired.

static std::vector<std::string> EnumerateModFolders() {
    std::vector<std::string> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path modsDir = fs::path("mods");
    if (!fs::exists(modsDir, ec) || !fs::is_directory(modsDir, ec)) return out;
    for (auto& entry : fs::directory_iterator(modsDir, ec)) {
        if (ec) break;
        if (entry.is_directory(ec)) {
            out.push_back(entry.path().filename().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::set<std::string> LoadEnabledMods() {
    std::set<std::string> enabled;
    FILE* f = fopen("mods.cfg", "r");
    if (!f) return enabled;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // trim trailing whitespace/newlines
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = 0;
        if (n > 0 && line[0] != '#') enabled.insert(std::string(line));
    }
    fclose(f);
    return enabled;
}

static void SaveEnabledMods(const std::vector<std::string>& folders,
                            const std::set<std::string>& enabled) {
    FILE* f = fopen("mods.cfg", "w");
    if (!f) return;
    fprintf(f, "# OpenCarnivores enabled mod list. One folder name per line.\n");
    fprintf(f, "# Lines are applied top-to-bottom (top = highest priority).\n");
    // Preserve the display order from the folders list for determinism.
    for (auto& name : folders) {
        if (enabled.count(name)) fprintf(f, "%s\n", name.c_str());
    }
    fclose(f);
}

// Enumerate subdirectories of shaderpacks/ for the addons screen.
static std::vector<std::string> EnumerateShaderPacks() {
    std::vector<std::string> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::path("shaderpacks");
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;
    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (entry.is_directory(ec))
            out.push_back(entry.path().filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::set<std::string> LoadEnabledPacks() {
    std::set<std::string> enabled;
    FILE* f = fopen("shaderpacks\\packs.cfg", "r");
    if (!f) return enabled;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int n = (int)strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = 0;
        if (n > 0 && line[0] != '#') enabled.insert(std::string(line));
    }
    fclose(f);
    return enabled;
}

static void SaveEnabledPacks(const std::vector<std::string>& packs,
                             const std::set<std::string>& enabled) {
    FILE* f = fopen("shaderpacks\\packs.cfg", "w");
    if (!f) return;
    fprintf(f, "# OpenCarnivores shader pack load order. One pack name per line.\n");
    fprintf(f, "# Packs apply in order; later packs override earlier for shared keys.\n");
    for (auto& name : packs)
        if (enabled.count(name)) fprintf(f, "%s\n", name.c_str());
    fclose(f);
}

static void RunModsScreen(bool& appQuit) {
    // SOURCEPORT: unified addons screen — shows both mods/ directories and
    // shaderpacks/ directories so all non-engine content is managed in one place.
    std::vector<std::string> modFolders  = EnumerateModFolders();
    std::vector<std::string> packFolders = EnumerateShaderPacks();
    std::set<std::string>    enabledMods  = LoadEnabledMods();
    std::set<std::string>    enabledPacks = LoadEnabledPacks();

    const int nMods     = (int)modFolders.size();
    const int nPacks    = (int)packFolders.size();
    // focusIdx layout: 0..nMods-1 = mod entries, nMods..nMods+nPacks-1 = pack entries,
    // IDX_APPLY = nMods+nPacks, IDX_BACK = nMods+nPacks+1.
    const int IDX_APPLY = nMods + nPacks;
    const int IDX_BACK  = nMods + nPacks + 1;

    int focusIdx = 0;

    MenuScreen ms = {};
    LoadMenuScreen(ms, "HUNTDAT\\MENU\\MENUM.TGA", nullptr, nullptr);

    while (!appQuit) {
        if (!PollMenuEvents(appQuit)) break;
        CompositeMenu(ms);  // no map on this screen; padDX/padDY pass through untouched

        // Controller D-pad / stick: move focus through the item list
        if (gMI.padDY > 0) { if (focusIdx < IDX_BACK)  ++focusIdx; gMI.padDX = 0; gMI.padDY = 0; }
        if (gMI.padDY < 0) { if (focusIdx > 0)          --focusIdx; gMI.padDX = 0; gMI.padDY = 0; }

        // Layout constants
        const int colX     = WinW * 420 / 800;
        const int colRight = WinW * 780 / 800;
        const int listX    = colX;
        const int listY    = WinH * 140 / 600;
        const int rowH     = WinH * 28  / 600;

        // Button pixel bounds (needed for pre-pass hover detection below)
        const char* kBack  = "BACK";
        const char* kApply = "APPLY";
        int backW  = g_glRenderer->MeasureTextBig(kBack);
        int applyW = g_glRenderer->MeasureTextBig(kApply);
        int btnH   = WinH * 44 / 600;
        int gap    = WinW * 30 / 800;
        int backLX  = WinW - backW  - WinW * 20 / 800;
        int backLY  = WinH - btnH   - WinH * 12 / 600;
        int backX0  = backLX  - WinW * 6 / 800;
        int backY0  = backLY  - WinH * 4 / 600;
        int backX1  = backLX  + backW  + WinW * 6 / 800;
        int backY1  = backLY  + btnH;
        int applyLX = backLX - gap - applyW;
        int applyLY = backLY;
        int applyX0 = applyLX - WinW * 6 / 800;
        int applyY0 = backY0;
        int applyX1 = applyLX + applyW + WinW * 6 / 800;
        int applyY1 = backY1;

        // Pre-pass: update focusIdx from mouse position so mouse and controller
        // stay in sync. Runs before rendering so 'hot' is computed from final focusIdx.
        {
            int row = 0;
            if (nMods > 0) {
                row++;  // section header row (not interactive)
                for (int i = 0; i < nMods; ++i, ++row) {
                    int ry = listY + row * rowH;
                    if (gMI.x >= listX && gMI.x < colRight && gMI.y >= ry && gMI.y < ry + rowH)
                        focusIdx = i;
                }
            }
            if (nPacks > 0) {
                if (nMods > 0) row++;   // blank gap row
                row++;                  // section header row
                for (int j = 0; j < nPacks; ++j, ++row) {
                    int ry = listY + row * rowH;
                    if (gMI.x >= listX && gMI.x < colRight && gMI.y >= ry && gMI.y < ry + rowH)
                        focusIdx = nMods + j;
                }
            }
            if (gMI.x >= backX0  && gMI.x < backX1  && gMI.y >= backY0  && gMI.y < backY1)  focusIdx = IDX_BACK;
            if (gMI.x >= applyX0 && gMI.x < applyX1 && gMI.y >= applyY0 && gMI.y < applyY1) focusIdx = IDX_APPLY;
        }

        MenuBegin();
        DrawMenuScreen(ms);
        MTBig("ADDONS", colX, WinH * 70 / 600, 0x00FFD040);

        // Rendering pass — hot is derived solely from focusIdx (updated by pre-pass above)
        if (nMods == 0 && nPacks == 0) {
            MT("No mods or shader packs installed.",       listX, listY,         0x00C0C0C0);
            MT("Mods: create a mods/<name>/ folder.",      listX, listY + rowH,   0x00909090);
            MT("Packs: create shaderpacks/<name>/ folder.",listX, listY + rowH*2, 0x00909090);
        } else {
            int row = 0;
            if (nMods > 0) {
                MT("-- MODS --", listX, listY + row * rowH, 0x00707070);
                row++;
                for (int i = 0; i < nMods; ++i, ++row) {
                    bool en  = enabledMods.count(modFolders[i]) > 0;
                    bool hot = (focusIdx == i);
                    uint32_t col = en ? 0x00FFE080 : 0x00A0A0A0;
                    if (hot) col = 0x00FFFFFF;
                    char line[320];
                    wsprintf(line, "[%s]  [M]  %s", en ? "X" : " ", modFolders[i].c_str());
                    MT(line, listX, listY + row * rowH, col);
                    if (hot && gMI.lClick) {
                        if (en) enabledMods.erase(modFolders[i]);
                        else    enabledMods.insert(modFolders[i]);
                        if (fxMenuGo.lpData) AddVoicev(fxMenuGo.length, fxMenuGo.lpData, 200);
                    }
                }
            }
            if (nPacks > 0) {
                if (nMods > 0) row++;  // blank gap
                MT("-- SHADER PACKS --", listX, listY + row * rowH, 0x00707070);
                row++;
                for (int j = 0; j < nPacks; ++j, ++row) {
                    bool en  = enabledPacks.count(packFolders[j]) > 0;
                    bool hot = (focusIdx == nMods + j);
                    uint32_t col = en ? 0x00FFE080 : 0x00A0A0A0;
                    if (hot) col = 0x00FFFFFF;
                    char line[320];
                    wsprintf(line, "[%s]  [SP] %s", en ? "X" : " ", packFolders[j].c_str());
                    MT(line, listX, listY + row * rowH, col);
                    if (hot && gMI.lClick) {
                        if (en) enabledPacks.erase(packFolders[j]);
                        else    enabledPacks.insert(packFolders[j]);
                        if (fxMenuGo.lpData) AddVoicev(fxMenuGo.length, fxMenuGo.lpData, 200);
                    }
                }
            }
        }

        bool backHot  = (focusIdx == IDX_BACK);
        bool applyHot = (focusIdx == IDX_APPLY);
        MTBig(kApply, applyLX, applyLY, applyHot ? 0x00FFE080 : 0x00AC6D24);
        MTBig(kBack,  backLX,  backLY,  backHot  ? 0x00FFE080 : 0x00AC6D24);

        bool doApply = gMI.lClick && applyHot;
        bool doBack  = (gMI.lClick && backHot) || gMI.scancode == SDL_SCANCODE_ESCAPE;

        MenuEnd();
        SDL_Delay(16);

        if (doApply) {
            SaveEnabledMods(modFolders, enabledMods);
            SaveEnabledPacks(packFolders, enabledPacks);
            FreeMenuScreen(ms);
            // SOURCEPORT: relaunch so VFS::Init and ShaderPackManager pick up new config.
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION procInfo = {};
            if (CreateProcessA(exePath, GetCommandLineA(), nullptr, nullptr,
                               FALSE, 0, nullptr, nullptr, &si, &procInfo)) {
                CloseHandle(procInfo.hProcess);
                CloseHandle(procInfo.hThread);
            }
            ExitProcess(0);
        }
        if (doBack) break;
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

    // SOURCEPORT: Trigger quad-pose re-anchor on the first menu frame so the
    // world-locked menu appears in front of the player's current gaze.
    s_menuFirstFrame = true;

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
            case 6: RunModsScreen(appQuit); break;   // Mods — SOURCEPORT
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
