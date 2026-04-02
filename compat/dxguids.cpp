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

// SOURCEPORT: Stub for DirectDrawCreate — the real implementation lives in
// ddraw.dll. This stub just returns E_FAIL. The D3D renderer will be replaced
// with OpenGL in Phase 3, so this code path will never be used in production.
extern "C" HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter) {
    (void)lpGUID; (void)lplpDD; (void)pUnkOuter;
    return E_FAIL;
}
