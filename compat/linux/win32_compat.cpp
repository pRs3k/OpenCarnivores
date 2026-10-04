// SOURCEPORT: Linux implementation of the Win32 subset declared in compat/linux/windows.h.
// See that header for the rationale. Compiled only on non-Windows builds.

#ifndef _WIN32

#include "windows.h"

#include <SDL.h>

#include <dirent.h>
#include <malloc.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

extern char** environ;

// ─────────────────────────────────────────────────────────────────────────────
// Command line: MSVC CRT exposes __argc/__argv; rebuild them from /proc so the
// game's own argument parser (Game.cpp ProcessCommandLine) runs unchanged.
// ─────────────────────────────────────────────────────────────────────────────
int    __argc = 0;
char** __argv = nullptr;

namespace {
std::vector<std::string> g_argStore;
std::vector<char*>       g_argPtrs;
std::string              g_cmdLine;

void LoadArgsFromProc() {
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g_argStore.clear();
    size_t start = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] == '\0') { g_argStore.emplace_back(all.substr(start, i - start)); start = i + 1; }
    }
    if (start < all.size()) g_argStore.emplace_back(all.substr(start));
    g_argPtrs.clear();
    g_cmdLine.clear();
    for (auto& a : g_argStore) {
        g_argPtrs.push_back(&a[0]);
        if (!g_cmdLine.empty()) g_cmdLine.push_back(' ');
        g_cmdLine += a;
    }
    g_argPtrs.push_back(nullptr);
    __argc = (int)g_argStore.size();
    __argv = g_argPtrs.data();
}

struct ArgsInit { ArgsInit() { LoadArgsFromProc(); } } g_argsInit;
} // namespace

LPSTR GetCommandLineA() { return &g_cmdLine[0]; }

// ─────────────────────────────────────────────────────────────────────────────
// Case-insensitive path resolution
// ─────────────────────────────────────────────────────────────────────────────
namespace {
std::mutex g_pathMutex;
std::unordered_map<std::string, std::string> g_pathCache;

bool Exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

std::string Join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string ResolveCI(const std::string& in) {
    std::string s = in;
    for (char& c : s) if (c == '\\') c = '/';
    if (s.empty() || Exists(s)) return s;

    std::lock_guard<std::mutex> lock(g_pathMutex);
    auto it = g_pathCache.find(s);
    if (it != g_pathCache.end()) return it->second;

    std::vector<std::string> comps;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            if (i > start) comps.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    std::string cur = (s[0] == '/') ? "/" : "";
    for (size_t i = 0; i < comps.size(); ++i) {
        const std::string& comp = comps[i];
        if (comp == ".") continue;
        std::string exact = Join(cur, comp);
        if (comp == ".." || Exists(exact)) { cur = exact; continue; }
        std::string found;
        if (DIR* d = opendir(cur.empty() ? "." : cur.c_str())) {
            while (dirent* e = readdir(d)) {
                if (strcasecmp(e->d_name, comp.c_str()) == 0) { found = e->d_name; break; }
            }
            closedir(d);
        }
        if (found.empty()) {
            // Not on disk (yet): keep the remaining components verbatim so writes
            // land where the caller asked, under the case-corrected parent.
            for (size_t j = i; j < comps.size(); ++j) cur = Join(cur, comps[j]);
            return cur;   // don't cache misses — the file may be created later
        }
        cur = Join(cur, found);
    }
    g_pathCache[s] = cur;
    return cur;
}
} // namespace

const char* OC_ResolvePath(const char* path) {
    // Ring of buffers so several resolved paths can live in one expression.
    static thread_local std::string ring[8];
    static thread_local int slot = 0;
    std::string& out = ring[slot++ & 7];
    out = ResolveCI(path ? path : "");
    return out.c_str();
}

// ─────────────────────────────────────────────────────────────────────────────
// File handles
// ─────────────────────────────────────────────────────────────────────────────
namespace {
constexpr uint32_t kFileMagic = 0x4F43464Cu; // 'OCFL'
constexpr uint32_t kHeapMagic = 0x4F434850u; // 'OCHP'
struct FileHandle { uint32_t magic; FILE* f; };
struct HeapHandle { uint32_t magic; };

FileHandle* AsFile(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return nullptr;
    auto* fh = static_cast<FileHandle*>(h);
    return fh->magic == kFileMagic ? fh : nullptr;
}
} // namespace

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD /*share*/, LPSECURITY_ATTRIBUTES,
                   DWORD disposition, DWORD /*flags*/, HANDLE /*templ*/) {
    if (!name || !*name) return INVALID_HANDLE_VALUE;
    std::string path = ResolveCI(name);
    const bool exists = Exists(path);
    const bool wantWrite = (access & GENERIC_WRITE) != 0;
    const char* mode = nullptr;
    switch (disposition) {
        case CREATE_NEW:        if (exists) return INVALID_HANDLE_VALUE; mode = "w+b"; break;
        case CREATE_ALWAYS:     mode = "w+b"; break;
        case OPEN_EXISTING:     if (!exists) return INVALID_HANDLE_VALUE;
                                mode = wantWrite ? "r+b" : "rb"; break;
        case OPEN_ALWAYS:       mode = exists ? (wantWrite ? "r+b" : "rb") : "w+b"; break;
        case TRUNCATE_EXISTING: if (!exists) return INVALID_HANDLE_VALUE; mode = "w+b"; break;
        default:                mode = wantWrite ? "w+b" : "rb"; break;
    }
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) return INVALID_HANDLE_VALUE;
    // Win32 WriteFile is unbuffered: keep render.log/crash.log complete if the
    // process dies, matching Windows behaviour. Reads stay stdio-buffered.
    if (wantWrite) setvbuf(f, nullptr, _IONBF, 0);
    return new FileHandle{kFileMagic, f};
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED) {
    FileHandle* fh = AsFile(h);
    if (!fh) { if (read) *read = 0; return FALSE; }
    size_t r = n ? std::fread(buf, 1, n, fh->f) : 0;
    if (read) *read = (DWORD)r;
    return TRUE;   // Win32 reports success with a short count at EOF
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED) {
    FileHandle* fh = AsFile(h);
    if (!fh) { if (written) *written = 0; return FALSE; }
    size_t w = n ? std::fwrite(buf, 1, n, fh->f) : 0;
    if (written) *written = (DWORD)w;
    return w == n;
}

DWORD SetFilePointer(HANDLE h, LONG dist, PLONG distHigh, DWORD method) {
    FileHandle* fh = AsFile(h);
    if (!fh) return INVALID_SET_FILE_POINTER;
    int64_t off = distHigh ? (((int64_t)*distHigh) << 32) | (uint32_t)dist : (int64_t)dist;
    int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_END ? SEEK_END : SEEK_CUR;
    if (fseeko(fh->f, (off_t)off, whence) != 0) return INVALID_SET_FILE_POINTER;
    int64_t pos = (int64_t)ftello(fh->f);
    if (distHigh) *distHigh = (LONG)(pos >> 32);
    return (DWORD)(pos & 0xFFFFFFFF);
}

DWORD GetFileSize(HANDLE h, LPDWORD high) {
    FileHandle* fh = AsFile(h);
    if (!fh) return INVALID_FILE_SIZE;
    struct stat st{};
    if (fstat(fileno(fh->f), &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)((uint64_t)st.st_size >> 32);
    return (DWORD)(st.st_size & 0xFFFFFFFF);
}

BOOL FlushFileBuffers(HANDLE h) {
    FileHandle* fh = AsFile(h);
    return fh && std::fflush(fh->f) == 0;
}

BOOL CloseHandle(HANDLE h) {
    if (FileHandle* fh = AsFile(h)) {
        std::fclose(fh->f);
        fh->magic = 0;
        delete fh;
        return TRUE;
    }
    return h && h != INVALID_HANDLE_VALUE;
}

BOOL DeleteFileA(LPCSTR name) { return unlink(OC_ResolvePath(name)) == 0; }

DWORD GetModuleFileNameA(HMODULE, LPSTR buf, DWORD size) {
    if (!buf || !size) return 0;
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n < 0) { buf[0] = 0; return 0; }
    buf[n] = 0;
    return (DWORD)n;
}

// ─────────────────────────────────────────────────────────────────────────────
// Heap
// ─────────────────────────────────────────────────────────────────────────────
HANDLE HeapCreate(DWORD, size_t, size_t) { static HeapHandle heap{kHeapMagic}; return &heap; }
LPVOID HeapAlloc(HANDLE, DWORD flags, size_t bytes) {
    if (bytes == 0) bytes = 1;
    return (flags & HEAP_ZERO_MEMORY) ? std::calloc(1, bytes) : std::malloc(bytes);
}
BOOL HeapFree(HANDLE, DWORD, LPVOID mem) { std::free(mem); return TRUE; }
// Only feeds the HeapReleased statistic; the usable size may exceed the request.
size_t HeapSize(HANDLE, DWORD, LPCVOID mem) { return mem ? malloc_usable_size(const_cast<void*>(mem)) : 0; }
BOOL HeapDestroy(HANDLE) { return TRUE; }

// ─────────────────────────────────────────────────────────────────────────────
// MSVC CRT rand(): holdrand = holdrand * 214013 + 2531011; return (holdrand >> 16) & 0x7FFF.
// Per-process (MSVC keeps it per-thread; the game only calls it from the main thread).
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t g_holdrand = 1;
void oc_msvc_srand(unsigned seed) { g_holdrand = seed; }
int  oc_msvc_rand() {
    g_holdrand = g_holdrand * 214013u + 2531011u;
    return (int)((g_holdrand >> 16) & 0x7FFF);
}

// ─────────────────────────────────────────────────────────────────────────────
// Strings / time / process
// ─────────────────────────────────────────────────────────────────────────────
int wsprintfA(LPSTR buf, LPCSTR fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    // wsprintf's documented limit is 1024 chars; callers size buffers for that.
    int n = vsnprintf(buf, 1024, fmt, ap);
    va_end(ap);
    return n;
}

DWORD timeGetTime() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
DWORD GetTickCount() { return timeGetTime(); }
void  Sleep(DWORD ms) { usleep((useconds_t)ms * 1000u); }

void GetLocalTime(SYSTEMTIME* st) {
    if (!st) return;
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm t{};
    localtime_r(&ts.tv_sec, &t);
    st->wYear = (WORD)(t.tm_year + 1900); st->wMonth  = (WORD)(t.tm_mon + 1);
    st->wDayOfWeek = (WORD)t.tm_wday;     st->wDay    = (WORD)t.tm_mday;
    st->wHour = (WORD)t.tm_hour;          st->wMinute = (WORD)t.tm_min;
    st->wSecond = (WORD)t.tm_sec;         st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}

[[noreturn]] void ExitProcess(UINT code) { std::exit((int)code); }

BOOL CreateProcessA(LPCSTR app, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                    BOOL, DWORD, LPVOID, LPCSTR, STARTUPINFOA*, PROCESS_INFORMATION* pi) {
    // Used to relaunch the game (restart after settings changes): re-exec with the
    // original argv. Command-line string parsing is not needed for that case.
    std::string exe = app ? app : "/proc/self/exe";
    pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, __argv, environ) != 0) return FALSE;
    if (pi) { pi->hProcess = pi->hThread = nullptr; pi->dwProcessId = (DWORD)pid; pi->dwThreadId = 0; }
    return TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic libraries
// ─────────────────────────────────────────────────────────────────────────────
HMODULE LoadLibraryA(LPCSTR name) {
    if (!name) return nullptr;
    // Map the Windows DLL names the engine probes to their Linux sonames.
    std::vector<std::string> candidates;
    if (strcasecmp(name, "openxr_loader.dll") == 0) {
        candidates = {"libopenxr_loader.so.1", "libopenxr_loader.so"};
    } else {
        std::string n = name;
        if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".dll") == 0) {
            // Windows-only DLLs (xinput, legacy audio drivers) have no Linux equivalent.
            return nullptr;
        }
        candidates = {n};
    }
    for (auto& c : candidates)
        if (void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
}
FARPROC GetProcAddress(HMODULE mod, LPCSTR name) {
    return mod ? reinterpret_cast<FARPROC>(dlsym(mod, name)) : nullptr;
}
BOOL FreeLibrary(HMODULE mod) { return mod && dlclose(mod) == 0; }

// ─────────────────────────────────────────────────────────────────────────────
// Message boxes, cursor, keyboard state
// ─────────────────────────────────────────────────────────────────────────────
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    std::fprintf(stderr, "[%s] %s\n", caption ? caption : "", text ? text : "");
    Uint32 flags = (type & 0xF0) == MB_ICONERROR ? SDL_MESSAGEBOX_ERROR
                 : (type & 0xF0) == MB_ICONEXCLAMATION ? SDL_MESSAGEBOX_WARNING
                 : SDL_MESSAGEBOX_INFORMATION;
    SDL_ShowSimpleMessageBox(flags, caption ? caption : "", text ? text : "", nullptr);
    return IDOK;
}

int GetSystemMetrics(int idx) {
    SDL_DisplayMode dm{};
    if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetDesktopDisplayMode(0, &dm) == 0)
        return idx == SM_CXSCREEN ? dm.w : dm.h;
    return idx == SM_CXSCREEN ? 1920 : 1080;
}

int ShowCursor(BOOL show) {
    // Win32 keeps a display counter; callers loop `while (ShowCursor(FALSE) >= 0)`.
    static int counter = 0;
    counter += show ? 1 : -1;
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_ShowCursor(counter >= 0 ? SDL_ENABLE : SDL_DISABLE);
    return counter;
}

BOOL SetCursorPos(int x, int y) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return FALSE;
    if (SDL_Window* w = SDL_GetMouseFocus()) SDL_WarpMouseInWindow(w, x, y);
    else SDL_WarpMouseGlobal(x, y);
    return TRUE;
}

BOOL GetCursorPos(LPPOINT p) {
    if (!p) return FALSE;
    int x = 0, y = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO)) SDL_GetMouseState(&x, &y);
    p->x = x; p->y = y;
    return TRUE;
}

namespace {
SDL_Scancode VKToScancode(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (vk - '1'));
    if (vk == '0') return SDL_SCANCODE_0;
    if (vk >= VK_F1 && vk <= VK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (vk - VK_F1));
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (vk - VK_NUMPAD1));
    switch (vk) {
        case VK_NUMPAD0:  return SDL_SCANCODE_KP_0;
        case VK_BACK:     return SDL_SCANCODE_BACKSPACE;
        case VK_TAB:      return SDL_SCANCODE_TAB;
        case VK_RETURN:   return SDL_SCANCODE_RETURN;
        case VK_SHIFT:
        case VK_LSHIFT:   return SDL_SCANCODE_LSHIFT;
        case VK_RSHIFT:   return SDL_SCANCODE_RSHIFT;
        case VK_CONTROL:
        case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
        case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
        case VK_MENU:
        case VK_LMENU:    return SDL_SCANCODE_LALT;
        case VK_RMENU:    return SDL_SCANCODE_RALT;
        case VK_PAUSE:    return SDL_SCANCODE_PAUSE;
        case VK_CAPITAL:  return SDL_SCANCODE_CAPSLOCK;
        case VK_ESCAPE:   return SDL_SCANCODE_ESCAPE;
        case VK_SPACE:    return SDL_SCANCODE_SPACE;
        case VK_PRIOR:    return SDL_SCANCODE_PAGEUP;
        case VK_NEXT:     return SDL_SCANCODE_PAGEDOWN;
        case VK_END:      return SDL_SCANCODE_END;
        case VK_HOME:     return SDL_SCANCODE_HOME;
        case VK_LEFT:     return SDL_SCANCODE_LEFT;
        case VK_UP:       return SDL_SCANCODE_UP;
        case VK_RIGHT:    return SDL_SCANCODE_RIGHT;
        case VK_DOWN:     return SDL_SCANCODE_DOWN;
        case VK_INSERT:   return SDL_SCANCODE_INSERT;
        case VK_DELETE:   return SDL_SCANCODE_DELETE;
        case VK_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
        case VK_ADD:      return SDL_SCANCODE_KP_PLUS;
        case VK_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
        case VK_DECIMAL:  return SDL_SCANCODE_KP_PERIOD;
        case VK_DIVIDE:   return SDL_SCANCODE_KP_DIVIDE;
        case VK_NUMLOCK:  return SDL_SCANCODE_NUMLOCKCLEAR;
        case VK_SCROLL:   return SDL_SCANCODE_SCROLLLOCK;
        case VK_OEM_1:      return SDL_SCANCODE_SEMICOLON;
        case VK_OEM_PLUS:   return SDL_SCANCODE_EQUALS;
        case VK_OEM_COMMA:  return SDL_SCANCODE_COMMA;
        case VK_OEM_MINUS:  return SDL_SCANCODE_MINUS;
        case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
        case VK_OEM_2:      return SDL_SCANCODE_SLASH;
        case VK_OEM_3:      return SDL_SCANCODE_GRAVE;
        case VK_OEM_4:      return SDL_SCANCODE_LEFTBRACKET;
        case VK_OEM_5:      return SDL_SCANCODE_BACKSLASH;
        case VK_OEM_6:      return SDL_SCANCODE_RIGHTBRACKET;
        case VK_OEM_7:      return SDL_SCANCODE_APOSTROPHE;
        default:            return SDL_SCANCODE_UNKNOWN;
    }
}
} // namespace

SHORT GetAsyncKeyState(int vk) {
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    SDL_PumpEvents();
    if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON) {
        Uint32 b = SDL_GetMouseState(nullptr, nullptr);
        Uint32 mask = vk == VK_LBUTTON ? SDL_BUTTON_LMASK
                    : vk == VK_RBUTTON ? SDL_BUTTON_RMASK : SDL_BUTTON_MMASK;
        return (b & mask) ? (SHORT)0x8000 : 0;
    }
    SDL_Scancode sc = VKToScancode(vk);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    const Uint8* ks = SDL_GetKeyboardState(nullptr);
    bool down = ks[sc] != 0;
    if (vk == VK_SHIFT)   down = down || ks[SDL_SCANCODE_RSHIFT];
    if (vk == VK_CONTROL) down = down || ks[SDL_SCANCODE_RCTRL];
    if (vk == VK_MENU)    down = down || ks[SDL_SCANCODE_RALT];
    return down ? (SHORT)0x8000 : 0;
}
SHORT GetKeyState(int vk) { return GetAsyncKeyState(vk); }

UINT MapVirtualKeyA(UINT code, UINT) { return code & 0xFF; }

int GetKeyNameTextA(LONG lParam, LPSTR buf, int size) {
    if (!buf || size <= 0) return 0;
    int vk = (int)((lParam >> 16) & 0xFF);
    SDL_Scancode sc = VKToScancode(vk);
    const char* name = sc != SDL_SCANCODE_UNKNOWN ? SDL_GetScancodeName(sc) : "";
    if (!name || !*name) { buf[0] = 0; return 0; }
    std::snprintf(buf, (size_t)size, "%s", name);
    return (int)strlen(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// GDI subset — text rasterised with stb_truetype into DIB sections
// ─────────────────────────────────────────────────────────────────────────────
namespace {
enum : int { kGdiFont = 1, kGdiBitmap = 2, kGdiStock = 3 };

struct FontFile {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    float avgCharWidthUnits = 0.f;   // OS/2 xAvgCharWidth (Win32 tmAveCharWidth basis)
    bool  ok = false;
};

uint16_t ReadU16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t ReadU32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

float ReadAvgCharWidth(const std::vector<unsigned char>& d) {
    if (d.size() < 12) return 0.f;
    uint16_t numTables = ReadU16(&d[4]);
    for (uint16_t i = 0; i < numTables; ++i) {
        size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > d.size()) break;
        if (memcmp(&d[rec], "OS/2", 4) == 0) {
            uint32_t off = ReadU32(&d[rec + 8]);
            if (off + 4 <= d.size()) return (float)(int16_t)ReadU16(&d[off + 2]);
        }
    }
    return 0.f;
}

FontFile* LoadFontFile(bool bold) {
    static FontFile files[2];
    static bool tried[2] = {false, false};
    FontFile& ff = files[bold ? 1 : 0];
    if (tried[bold ? 1 : 0]) return ff.ok ? &ff : nullptr;
    tried[bold ? 1 : 0] = true;

    std::vector<std::string> cands;
    if (const char* env = getenv(bold ? "OPENCARNIVORES_FONT_BOLD" : "OPENCARNIVORES_FONT")) cands.push_back(env);
    const char* reg[] = {
        "fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    const char* bld[] = {
        "fonts/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    };
    for (const char* c : (bold ? bld : reg)) cands.push_back(c);

    for (auto& path : cands) {
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        ff.data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (ff.data.empty()) continue;
        if (!stbtt_InitFont(&ff.info, ff.data.data(), stbtt_GetFontOffsetForIndex(ff.data.data(), 0))) continue;
        ff.avgCharWidthUnits = ReadAvgCharWidth(ff.data);
        ff.ok = true;
        return &ff;
    }
    if (bold) return LoadFontFile(false);   // no bold face installed — use regular
    std::fprintf(stderr, "[compat] no TrueType font found (install fonts-liberation or set OPENCARNIVORES_FONT)\n");
    return nullptr;
}
} // namespace

struct OCFontObj {
    int   kind = kGdiFont;
    FontFile* file = nullptr;
    float scaleX = 1.f, scaleY = 1.f;
    int   ascentPx = 0, cellHeightPx = 0;
};

struct OCBitmapObj {
    int   kind = kGdiBitmap;
    int   w = 0, h = 0, bpp = 0, stride = 0;
    bool  topDown = false;
    std::vector<unsigned char> bits;
    unsigned char* Row(int y) { return bits.data() + (size_t)(topDown ? y : (h - 1 - y)) * stride; }
};

struct OCDeviceContext {
    OCFontObj*   font = nullptr;
    OCBitmapObj* bmp  = nullptr;
    COLORREF     textColor = 0;
    COLORREF     bkColor   = 0xFFFFFF;
    int          bkMode    = OPAQUE;
};

namespace {
// GDI lays text out on whole pixels (hinted integer advances). Matching that keeps
// 1-px stems (i, l, t) on a single column instead of two half-intensity ones.
int GlyphAdvancePx(const OCFontObj* f, int cp) {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->file->info, cp, &adv, &lsb);
    return std::max(1, (int)std::lround(adv * f->scaleX));
}

// Unhinted small-size coverage is pale next to GDI's hinted glyphs; a mild gamma
// restores stem weight without hardening the anti-aliased edges.
const unsigned char* CoverageLUT() {
    static unsigned char lut[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i)
            lut[i] = (unsigned char)std::lround(255.0 * std::pow(i / 255.0, 0.6));
        init = true;
    }
    return lut;
}
} // namespace

HFONT CreateFontA(int h, int w, int, int, int weight, DWORD, DWORD, DWORD, DWORD, DWORD,
                  DWORD, DWORD, DWORD, LPCSTR) {
    FontFile* ff = LoadFontFile(weight >= 600);
    auto* f = new OCFontObj;
    f->file = ff;
    if (!ff) return f;
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&ff->info, &asc, &desc, &gap);
    // Win32: h > 0 is the cell height (ascent + descent), h < 0 the em height, 0 = default.
    if (h == 0) h = 16;
    f->scaleY = h > 0 ? stbtt_ScaleForPixelHeight(&ff->info, (float)h)
                      : stbtt_ScaleForMappingEmToPixels(&ff->info, (float)-h);
    // Win32: w > 0 is the average character width; scale glyphs horizontally to match.
    float avgUnits = ff->avgCharWidthUnits;
    if (avgUnits <= 0.f) { int adv = 0, lsb = 0; stbtt_GetCodepointHMetrics(&ff->info, 'x', &adv, &lsb); avgUnits = (float)adv; }
    f->scaleX = (w > 0 && avgUnits > 0.f) ? (float)w / avgUnits : f->scaleY;
    f->ascentPx     = (int)std::lround(asc * f->scaleY);
    f->cellHeightPx = (int)std::lround((asc - desc) * f->scaleY);
    return f;
}

HDC  CreateCompatibleDC(HDC) { return new OCDeviceContext; }
BOOL DeleteDC(HDC dc) { delete dc; return TRUE; }
HDC  GetDC(HWND) { static OCDeviceContext screen; return &screen; }
int  ReleaseDC(HWND, HDC) { return 1; }

HBITMAP CreateDIBSection(HDC, const BITMAPINFO* bi, UINT, void** bits, HANDLE, DWORD) {
    if (!bi || bi->bmiHeader.biWidth <= 0 || bi->bmiHeader.biHeight == 0) return nullptr;
    auto* b = new OCBitmapObj;
    b->w = bi->bmiHeader.biWidth;
    b->h = bi->bmiHeader.biHeight < 0 ? -bi->bmiHeader.biHeight : bi->bmiHeader.biHeight;
    b->topDown = bi->bmiHeader.biHeight < 0;
    b->bpp = bi->bmiHeader.biBitCount ? bi->bmiHeader.biBitCount : 24;
    b->stride = ((b->w * b->bpp + 31) / 32) * 4;
    b->bits.assign((size_t)b->stride * b->h, 0);
    if (bits) *bits = b->bits.data();
    return b;
}

HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj) {
    if (!dc || !obj) return nullptr;
    int kind = *static_cast<int*>(obj);
    if (kind == kGdiFont)   { HGDIOBJ old = dc->font; dc->font = static_cast<OCFontObj*>(obj); return old; }
    if (kind == kGdiBitmap) { HGDIOBJ old = dc->bmp;  dc->bmp  = static_cast<OCBitmapObj*>(obj); return old; }
    return nullptr;
}

BOOL DeleteObject(HGDIOBJ obj) {
    if (!obj) return FALSE;
    int kind = *static_cast<int*>(obj);
    if (kind == kGdiFont)   delete static_cast<OCFontObj*>(obj);
    if (kind == kGdiBitmap) delete static_cast<OCBitmapObj*>(obj);
    return TRUE;
}

HGDIOBJ GetStockObject(int) { static int stock = kGdiStock; return &stock; }

int SetBkMode(HDC dc, int mode) { if (!dc) return 0; int o = dc->bkMode; dc->bkMode = mode; return o; }
COLORREF SetTextColor(HDC dc, COLORREF c) { if (!dc) return 0; COLORREF o = dc->textColor; dc->textColor = c; return o; }
COLORREF SetBkColor(HDC dc, COLORREF c) { if (!dc) return 0; COLORREF o = dc->bkColor; dc->bkColor = c; return o; }

BOOL GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz) {
    if (!sz) return FALSE;
    sz->cx = sz->cy = 0;
    if (!dc || !dc->font || !dc->font->file || !s) return FALSE;
    OCFontObj* f = dc->font;
    int x = 0;
    for (int i = 0; i < n; ++i) x += GlyphAdvancePx(f, (unsigned char)s[i]);
    sz->cx = (LONG)x;
    sz->cy = f->cellHeightPx;
    return TRUE;
}

BOOL TextOutA(HDC dc, int x, int y, LPCSTR s, int n) {
    if (!dc || !dc->font || !dc->font->file || !dc->bmp || !s) return FALSE;
    OCFontObj* f = dc->font;
    OCBitmapObj* b = dc->bmp;
    if (b->bpp != 24 && b->bpp != 32) return FALSE;
    const int bytesPP = b->bpp / 8;
    const unsigned char tr = (unsigned char)(dc->textColor & 0xFF);
    const unsigned char tg = (unsigned char)((dc->textColor >> 8) & 0xFF);
    const unsigned char tb = (unsigned char)((dc->textColor >> 16) & 0xFF);

    if (dc->bkMode == OPAQUE) {
        SIZE ext{};
        GetTextExtentPoint32A(dc, s, n, &ext);
        const unsigned char br = (unsigned char)(dc->bkColor & 0xFF);
        const unsigned char bg = (unsigned char)((dc->bkColor >> 8) & 0xFF);
        const unsigned char bb = (unsigned char)((dc->bkColor >> 16) & 0xFF);
        for (int py = std::max(0, y); py < std::min(b->h, y + (int)ext.cy); ++py) {
            unsigned char* row = b->Row(py);
            for (int px = std::max(0, x); px < std::min(b->w, x + (int)ext.cx); ++px) {
                unsigned char* p = row + px * bytesPP;
                p[0] = bb; p[1] = bg; p[2] = br;
            }
        }
    }

    int penX = x;
    const int baseline = y + f->ascentPx;
    const unsigned char* lut = CoverageLUT();
    std::vector<unsigned char> glyph;
    for (int i = 0; i < n; ++i) {
        int cp = (unsigned char)s[i];
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(&f->file->info, cp, f->scaleX, f->scaleY, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            glyph.assign((size_t)gw * gh, 0);
            stbtt_MakeCodepointBitmap(&f->file->info, glyph.data(), gw, gh, gw,
                                      f->scaleX, f->scaleY, cp);
            int ox = penX + x0;
            int oy = baseline + y0;
            for (int gy = 0; gy < gh; ++gy) {
                int py = oy + gy;
                if (py < 0 || py >= b->h) continue;
                unsigned char* row = b->Row(py);
                for (int gx = 0; gx < gw; ++gx) {
                    int px = ox + gx;
                    if (px < 0 || px >= b->w) continue;
                    unsigned a = lut[glyph[(size_t)gy * gw + gx]];
                    if (!a) continue;
                    unsigned char* p = row + px * bytesPP;   // DIB pixels are BGR(A)
                    p[0] = (unsigned char)((tb * a + p[0] * (255 - a)) / 255);
                    p[1] = (unsigned char)((tg * a + p[1] * (255 - a)) / 255);
                    p[2] = (unsigned char)((tr * a + p[2] * (255 - a)) / 255);
                }
            }
        }
        penX += GlyphAdvancePx(f, cp);
    }
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD) {
    if (!dst || !src || !dst->bmp || !src->bmp || dst->bmp->bpp != src->bmp->bpp) return FALSE;
    OCBitmapObj* d = dst->bmp;
    OCBitmapObj* s = src->bmp;
    const int bytesPP = d->bpp / 8;
    for (int j = 0; j < h; ++j) {
        int dy = y + j, syy = sy + j;
        if (dy < 0 || dy >= d->h || syy < 0 || syy >= s->h) continue;
        for (int i = 0; i < w; ++i) {
            int dx = x + i, sxx = sx + i;
            if (dx < 0 || dx >= d->w || sxx < 0 || sxx >= s->w) continue;
            memcpy(d->Row(dy) + dx * bytesPP, s->Row(syy) + sxx * bytesPP, (size_t)bytesPP);
        }
    }
    return TRUE;
}

#endif // !_WIN32
