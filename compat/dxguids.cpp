// SOURCEPORT: Defines the DirectX GUIDs and stub functions that the original
// code references. Normally these come from dxguid.lib and ddraw.lib, but
// since we use stub headers we define them here.
#include <initguid.h>
#include <objbase.h>
#include "ddraw.h"

// IID_IDirect3D  {3BBA0080-2421-11CF-A31A-00AA00B93356}
DEFINE_GUID(IID_IDirect3D,
    0x3BBA0080, 0x2421, 0x11CF, 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);

// IID_IDirect3DTexture  {2CDCD9E0-25A0-11CF-A31A-00AA00B93356}
DEFINE_GUID(IID_IDirect3DTexture,
    0x2CDCD9E0, 0x25A0, 0x11CF, 0xA3, 0x1A, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56);

// SOURCEPORT: Load DirectDrawCreate from the real ddraw.dll at runtime.
// Windows 10 still ships ddraw.dll for backward compatibility.
// This avoids needing to link against ddraw.lib from the legacy DirectX SDK.
typedef HRESULT (WINAPI *PFN_DirectDrawCreate)(GUID*, LPDIRECTDRAW*, IUnknown*);

static PFN_DirectDrawCreate pfnDirectDrawCreate = nullptr;

static bool LoadDirectDraw() {
    if (pfnDirectDrawCreate) return true;
    HMODULE hDDraw = LoadLibraryA("ddraw.dll");
    if (!hDDraw) return false;
    pfnDirectDrawCreate = (PFN_DirectDrawCreate)GetProcAddress(hDDraw, "DirectDrawCreate");
    return pfnDirectDrawCreate != nullptr;
}

extern "C" HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter) {
    if (!LoadDirectDraw()) return E_FAIL;
    return pfnDirectDrawCreate(lpGUID, lplpDD, pUnkOuter);
}
