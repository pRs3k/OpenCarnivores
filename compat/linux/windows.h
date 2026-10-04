// SOURCEPORT: Linux shim for the subset of <windows.h> the OpenGL/SDL2 build uses.
//
// Only on the include path for non-Windows builds (see CMakeLists.txt). Game
// sources keep calling the Win32 names; this header maps them onto POSIX/SDL:
//   - Win32 scalar/handle types with Windows (LLP64) sizes — DWORD/LONG stay 32-bit
//     so retail binary loaders (.CAR/.3DF/.RSC/.MAP/.TGA/.WAV) read identical layouts.
//   - CreateFile/ReadFile/WriteFile/SetFilePointer → stdio, with backslash and
//     case-insensitive path resolution (retail assets mix "HUNTDAT\\ship2a.car" casing).
//   - GDI text (CreateFont/TextOut/GetTextExtentPoint32 into a DIB) → stb_truetype
//     with Liberation Sans (Arial-metric) / DejaVu Sans.
//   - Heap*, timeGetTime, Sleep, MessageBox, LoadLibrary/GetProcAddress (dlopen).
// Implementation: compat/linux/win32_compat.cpp.
#pragma once

#ifndef _WIN32

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <strings.h>
#include <algorithm>
#include <type_traits>

// ── Calling conventions / attributes ─────────────────────────────────────────
#define WINAPI
#define APIENTRY
#define CALLBACK
#define PASCAL
#define __stdcall
#define __cdecl
#define _cdecl

// ── Scalar types (Windows LLP64 sizes) ───────────────────────────────────────
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef int      BOOL;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int16_t  SHORT;
typedef uint32_t UINT;
typedef int32_t  HRESULT;
typedef char     CHAR;
typedef uint32_t COLORREF;
typedef intptr_t  LONG_PTR;
typedef uintptr_t UINT_PTR;
typedef uintptr_t WPARAM;
typedef intptr_t  LPARAM;
typedef intptr_t  LRESULT;
typedef int64_t  __int64;
typedef unsigned char byte;   // rpcndr.h

typedef char*       LPSTR;
typedef const char* LPCSTR;
typedef void*       LPVOID;
typedef const void* LPCVOID;
typedef DWORD*      LPDWORD;
typedef BYTE*       LPBYTE;
typedef WORD*       LPWORD;
typedef LONG*       PLONG;
typedef void*       LPSECURITY_ATTRIBUTES;
typedef void*       LPOVERLAPPED;

// ── Handles ──────────────────────────────────────────────────────────────────
typedef void* HANDLE;
typedef void* HWND;
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef void* HCURSOR;
typedef void* HICON;
typedef void* HBRUSH;
typedef void* HPEN;
typedef void* HGLRC;
typedef void* HGDIOBJ;
struct OCDeviceContext;
struct OCFontObj;
struct OCBitmapObj;
typedef OCDeviceContext* HDC;
typedef OCFontObj*       HFONT;
typedef OCBitmapObj*     HBITMAP;
typedef int (*FARPROC)();

#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#define S_OK      ((HRESULT)0)
#define E_FAIL    ((HRESULT)0x80004005L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) < 0)

#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xffff))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))
#define RGB(r,g,b) ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))

// The 1999 sources use the Win32 min/max macros on mixed int/float operands.
#ifndef NOMINMAX
// Return by value (std::common_type): `decltype(a < b ? a : b)` would be a
// dangling reference to a parameter when both operands share a type.
template<class A, class B> inline typename std::common_type<A, B>::type min(A a, B b) { return a < b ? a : b; }
template<class A, class B> inline typename std::common_type<A, B>::type max(A a, B b) { return a > b ? a : b; }
#endif

// ── MSVC-compatible rand() ───────────────────────────────────────────────────
// The 1999 code assumes MSVC's 15-bit rand(): expressions such as
// `rand() * (R*2+1) / RAND_MAX` (siRand) and `rand() * 1024 / RAND_MAX` overflow
// int with glibc's 31-bit RAND_MAX, scattering spawns/objects off the map.
// Reproduce the MSVC CRT generator exactly so sequences match the Windows build.
#include <stdlib.h>   // set its include guard before the macros below
void oc_msvc_srand(unsigned seed);
int  oc_msvc_rand();
// Later `std::rand` / `using std::rand;` (libstdc++ headers) expand to these names.
namespace std { using ::oc_msvc_rand; using ::oc_msvc_srand; }
#undef  RAND_MAX
#define RAND_MAX 0x7FFF
#define rand  oc_msvc_rand
#define srand oc_msvc_srand

// ── Structs ──────────────────────────────────────────────────────────────────
struct POINT { LONG x, y; };
typedef POINT* LPPOINT;
struct SIZE  { LONG cx, cy; };
typedef SIZE* LPSIZE;
struct RECT  { LONG left, top, right, bottom; };
typedef RECT* LPRECT;
struct MSG   { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; };

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

#pragma pack(push, 1)
struct BITMAPFILEHEADER {
    WORD  bfType;
    DWORD bfSize;
    WORD  bfReserved1;
    WORD  bfReserved2;
    DWORD bfOffBits;
};
#pragma pack(pop)
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };
#define BI_RGB         0
#define DIB_RGB_COLORS 0

// ── File I/O ─────────────────────────────────────────────────────────────────
#define GENERIC_READ          0x80000000u
#define GENERIC_WRITE         0x40000000u
#define FILE_SHARE_READ       0x00000001u
#define FILE_SHARE_WRITE      0x00000002u
#define CREATE_NEW            1
#define CREATE_ALWAYS         2
#define OPEN_EXISTING         3
#define OPEN_ALWAYS           4
#define TRUNCATE_EXISTING     5
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_BEGIN            0
#define FILE_CURRENT          1
#define FILE_END              2
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define INVALID_FILE_SIZE        ((DWORD)0xFFFFFFFF)

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                   DWORD disposition, DWORD flags, HANDLE templ);
#define CreateFile CreateFileA
BOOL  ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov);
BOOL  WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD written, LPOVERLAPPED ov);
DWORD SetFilePointer(HANDLE h, LONG dist, PLONG distHigh, DWORD method);
DWORD GetFileSize(HANDLE h, LPDWORD high);
BOOL  CloseHandle(HANDLE h);
BOOL  DeleteFileA(LPCSTR name);
#define DeleteFile DeleteFileA
BOOL  FlushFileBuffers(HANDLE h);
DWORD GetModuleFileNameA(HMODULE mod, LPSTR buf, DWORD size);
LPSTR GetCommandLineA();

// SOURCEPORT: resolve a game path on a case-sensitive filesystem: backslashes →
// '/', then match each component case-insensitively. Returns the input
// (slash-normalised) when nothing matches, so create/write paths still work.
const char* OC_ResolvePath(const char* path);

// ── Memory ───────────────────────────────────────────────────────────────────
#define HEAP_NO_SERIALIZE 0x00000001
#define HEAP_ZERO_MEMORY  0x00000008
HANDLE HeapCreate(DWORD opts, size_t initial, size_t maximum);
LPVOID HeapAlloc(HANDLE heap, DWORD flags, size_t bytes);
BOOL   HeapFree(HANDLE heap, DWORD flags, LPVOID mem);
size_t HeapSize(HANDLE heap, DWORD flags, LPCVOID mem);
BOOL   HeapDestroy(HANDLE heap);
#define ZeroMemory(d, n)      memset((d), 0, (n))
#define FillMemory(d, n, v)   memset((d), (v), (n))
#define CopyMemory(d, s, n)   memcpy((d), (s), (n))
#define MoveMemory(d, s, n)   memmove((d), (s), (n))

// ── Strings ──────────────────────────────────────────────────────────────────
int wsprintfA(LPSTR buf, LPCSTR fmt, ...);
#define wsprintf wsprintfA
#define lstrlenA(s)    ((int)strlen(s))
#define lstrlen        lstrlenA
#define lstrcpyA(d,s)  strcpy((d),(s))
#define lstrcpy        lstrcpyA
#define lstrcatA(d,s)  strcat((d),(s))
#define lstrcat        lstrcatA
#define lstrcmpA(a,b)  strcmp((a),(b))
#define lstrcmp        lstrcmpA
#define lstrcmpiA(a,b) strcasecmp((a),(b))
#define lstrcmpi       lstrcmpiA
#define _stricmp       strcasecmp
#define stricmp        strcasecmp
#define _strnicmp      strncasecmp
#define strnicmp       strncasecmp
#define _strdup        strdup
#define _snprintf      snprintf
#define _vsnprintf     vsnprintf
#define OutputDebugStringA(s) ((void)fputs((s), stderr))
#define OutputDebugString     OutputDebugStringA

// ── Time / process ───────────────────────────────────────────────────────────
DWORD timeGetTime();
DWORD GetTickCount();
void  Sleep(DWORD ms);
void  GetLocalTime(SYSTEMTIME* st);
[[noreturn]] void ExitProcess(UINT code);  // DECLSPEC_NORETURN on Windows
#define timeBeginPeriod(x) ((void)0)
#define timeEndPeriod(x)   ((void)0)

// Command line (Game.cpp parses __argc/__argv like the MSVC CRT globals).
extern int    __argc;
extern char** __argv;

struct STARTUPINFOA { DWORD cb; };
struct PROCESS_INFORMATION { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; };
BOOL CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta,
                    BOOL inherit, DWORD flags, LPVOID env, LPCSTR dir,
                    STARTUPINFOA* si, PROCESS_INFORMATION* pi);

// ── Structured exceptions (crash handler is a no-op on Linux) ────────────────
struct EXCEPTION_RECORD { DWORD ExceptionCode; void* ExceptionAddress; };
struct EXCEPTION_POINTERS { EXCEPTION_RECORD* ExceptionRecord; void* ContextRecord; };
#define EXCEPTION_EXECUTE_HANDLER 1
typedef LONG (*LPTOP_LEVEL_EXCEPTION_FILTER)(EXCEPTION_POINTERS*);
inline LPTOP_LEVEL_EXCEPTION_FILTER SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER) { return nullptr; }

// ── Dynamic libraries ────────────────────────────────────────────────────────
HMODULE LoadLibraryA(LPCSTR name);
#define LoadLibrary LoadLibraryA
FARPROC GetProcAddress(HMODULE mod, LPCSTR name);
BOOL    FreeLibrary(HMODULE mod);

// ── Message boxes / window stubs (SDL owns the window) ───────────────────────
#define MB_OK              0x00000000
#define MB_OKCANCEL        0x00000001
#define MB_YESNO           0x00000004
#define MB_ICONERROR       0x00000010
#define MB_ICONSTOP        MB_ICONERROR
#define MB_ICONEXCLAMATION 0x00000030
#define MB_ICONWARNING     MB_ICONEXCLAMATION
#define MB_ICONINFORMATION 0x00000040
#define MB_SYSTEMMODAL     0x00001000
#define IDOK  1
#define IDYES 6
#define IDNO  7
int MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type);
#define MessageBox MessageBoxA
inline BOOL MessageBeep(UINT) { return TRUE; }

#define HWND_TOP       ((HWND)0)
#define SWP_SHOWWINDOW 0x0040
#define SM_CXSCREEN    0
#define SM_CYSCREEN    1
inline BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
inline BOOL EnableWindow(HWND, BOOL) { return TRUE; }
int  GetSystemMetrics(int idx);
int  ShowCursor(BOOL show);
inline HCURSOR SetCursor(HCURSOR) { return nullptr; }
BOOL SetCursorPos(int x, int y);
BOOL GetCursorPos(LPPOINT p);
SHORT GetAsyncKeyState(int vk);
SHORT GetKeyState(int vk);

// ── Virtual-key codes (Windows values: controls.cfg and KeyboardState[] use them) ─
#define VK_LBUTTON   0x01
#define VK_RBUTTON   0x02
#define VK_CANCEL    0x03
#define VK_MBUTTON   0x04
#define VK_BACK      0x08
#define VK_TAB       0x09
#define VK_RETURN    0x0D
#define VK_SHIFT     0x10
#define VK_CONTROL   0x11
#define VK_MENU      0x12
#define VK_PAUSE     0x13
#define VK_CAPITAL   0x14
#define VK_ESCAPE    0x1B
#define VK_SPACE     0x20
#define VK_PRIOR     0x21
#define VK_NEXT      0x22
#define VK_END       0x23
#define VK_HOME      0x24
#define VK_LEFT      0x25
#define VK_UP        0x26
#define VK_RIGHT     0x27
#define VK_DOWN      0x28
#define VK_SNAPSHOT  0x2C
#define VK_INSERT    0x2D
#define VK_DELETE    0x2E
#define VK_LWIN      0x5B
#define VK_RWIN      0x5C
#define VK_NUMPAD0   0x60
#define VK_NUMPAD1   0x61
#define VK_NUMPAD2   0x62
#define VK_NUMPAD3   0x63
#define VK_NUMPAD4   0x64
#define VK_NUMPAD5   0x65
#define VK_NUMPAD6   0x66
#define VK_NUMPAD7   0x67
#define VK_NUMPAD8   0x68
#define VK_NUMPAD9   0x69
#define VK_MULTIPLY  0x6A
#define VK_ADD       0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT  0x6D
#define VK_DECIMAL   0x6E
#define VK_DIVIDE    0x6F
#define VK_F1        0x70
#define VK_F2        0x71
#define VK_F3        0x72
#define VK_F4        0x73
#define VK_F5        0x74
#define VK_F6        0x75
#define VK_F7        0x76
#define VK_F8        0x77
#define VK_F9        0x78
#define VK_F10       0x79
#define VK_F11       0x7A
#define VK_F12       0x7B
#define VK_NUMLOCK   0x90
#define VK_SCROLL    0x91
#define VK_LSHIFT    0xA0
#define VK_RSHIFT    0xA1
#define VK_LCONTROL  0xA2
#define VK_RCONTROL  0xA3
#define VK_LMENU     0xA4
#define VK_RMENU     0xA5
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE

#define MAPVK_VK_TO_VSC 0
// Linux: "scan code" is the VK itself; GetKeyNameTextA decodes it back to a name.
UINT MapVirtualKeyA(UINT code, UINT mapType);
#define MapVirtualKey MapVirtualKeyA
int  GetKeyNameTextA(LONG lParam, LPSTR buf, int size);

// ── GDI subset: fonts, memory DCs, DIB sections, TextOut ─────────────────────
#define ANSI_CHARSET        0
#define DEFAULT_CHARSET     1
#define OUT_DEFAULT_PRECIS  0
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY     0
#define NONANTIALIASED_QUALITY 3
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY   5
#define DEFAULT_PITCH       0
#define FIXED_PITCH         1
#define VARIABLE_PITCH      2
#define FF_DONTCARE         0x00
#define FF_ROMAN            0x10
#define FF_SWISS            0x20
#define FF_MODERN           0x30
#define FW_NORMAL           400
#define FW_BOLD             700
#define TRANSPARENT         1
#define OPAQUE              2
#define SRCCOPY             0x00CC0020
#define BLACK_BRUSH         4
#define NULL_BRUSH          5

HFONT   CreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic,
                    DWORD underline, DWORD strike, DWORD charset, DWORD outPrec,
                    DWORD clipPrec, DWORD quality, DWORD pitchFamily, LPCSTR face);
#define CreateFont CreateFontA
HDC     CreateCompatibleDC(HDC dc);
BOOL    DeleteDC(HDC dc);
HDC     GetDC(HWND w);
int     ReleaseDC(HWND w, HDC dc);
HBITMAP CreateDIBSection(HDC dc, const BITMAPINFO* bi, UINT usage, void** bits,
                         HANDLE section, DWORD offset);
HGDIOBJ SelectObject(HDC dc, HGDIOBJ obj);
inline HGDIOBJ SelectObject(HDC dc, HFONT f)   { return SelectObject(dc, (HGDIOBJ)f); }
inline HGDIOBJ SelectObject(HDC dc, HBITMAP b) { return SelectObject(dc, (HGDIOBJ)b); }
BOOL    DeleteObject(HGDIOBJ obj);
inline BOOL DeleteObject(HFONT f)   { return DeleteObject((HGDIOBJ)f); }
inline BOOL DeleteObject(HBITMAP b) { return DeleteObject((HGDIOBJ)b); }
HGDIOBJ GetStockObject(int i);
int      SetBkMode(HDC dc, int mode);
COLORREF SetTextColor(HDC dc, COLORREF c);
COLORREF SetBkColor(HDC dc, COLORREF c);
BOOL    TextOutA(HDC dc, int x, int y, LPCSTR s, int n);
#define TextOut TextOutA
BOOL    GetTextExtentPoint32A(HDC dc, LPCSTR s, int n, LPSIZE sz);
#define GetTextExtentPoint32 GetTextExtentPoint32A
#define GetTextExtentPointA  GetTextExtentPoint32A
#define GetTextExtentPoint   GetTextExtentPoint32A
BOOL    BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);


#endif // !_WIN32
