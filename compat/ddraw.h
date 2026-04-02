// SOURCEPORT: Stub DirectDraw header for compiling Carnivores 2 with modern MSVC.
// Provides just enough type definitions and COM interface declarations for the
// original renderd3d.cpp code to compile. No actual DirectDraw functionality —
// the renderer will be replaced with OpenGL in Phase 3.
#pragma once

#include <windows.h>
#include <objbase.h>

// ============================================================
// Forward declarations
// ============================================================
struct IDirectDraw;
struct IDirectDraw2;
struct IDirectDrawSurface;

typedef IDirectDraw*        LPDIRECTDRAW;
typedef IDirectDraw2*       LPDIRECTDRAW2;
typedef IDirectDrawSurface* LPDIRECTDRAWSURFACE;

// ============================================================
// Return codes
// ============================================================
#ifndef DD_OK
#define DD_OK               S_OK
#endif
#ifndef DDERR_GENERIC
#define DDERR_GENERIC       E_FAIL
#endif
#ifndef DD_FALSE
#define DD_FALSE            S_FALSE
#endif

// ============================================================
// DirectDraw cooperative level flags
// ============================================================
#define DDSCL_FULLSCREEN        0x00000001
#define DDSCL_EXCLUSIVE         0x00000010
#define DDSCL_NORMAL            0x00000008

// ============================================================
// Surface description flags (DDSD_*)
// ============================================================
#define DDSD_CAPS               0x00000001
#define DDSD_HEIGHT             0x00000002
#define DDSD_WIDTH              0x00000004
#define DDSD_PIXELFORMAT        0x00001000
#define DDSD_ZBUFFERBITDEPTH    0x00000040

// ============================================================
// Surface capabilities (DDSCAPS_*)
// ============================================================
#define DDSCAPS_PRIMARYSURFACE  0x00000200
#define DDSCAPS_OFFSCREENPLAIN  0x00000040
#define DDSCAPS_3DDEVICE        0x00002000
#define DDSCAPS_VIDEOMEMORY     0x00004000
#define DDSCAPS_ZBUFFER         0x00000020
#define DDSCAPS_TEXTURE         0x00001000

// ============================================================
// Blt flags (DDBLT_*)
// ============================================================
#define DDBLT_COLORFILL         0x00000400
#define DDBLT_DEPTHFILL         0x02000000
#define DDBLT_WAIT              0x01000000

// ============================================================
// Color key flags (DDCKEY_*)
// ============================================================
#define DDCKEY_SRCBLT           0x00000008

// ============================================================
// Lock flags (DDLOCK_*)
// ============================================================
#define DDLOCK_WAIT             0x00000001

// ============================================================
// Pixel format flags (DDPF_*)
// ============================================================
#define DDPF_RGB                0x00000040
#define DDPF_ALPHAPIXELS        0x00000001

// ============================================================
// Bit depth flags (DDBD_*)
// ============================================================
#define DDBD_1      0x00004000
#define DDBD_2      0x00002000
#define DDBD_4      0x00001000
#define DDBD_8      0x00000800
#define DDBD_16     0x00000400
#define DDBD_24     0x00000200
#define DDBD_32     0x00000100

// ============================================================
// Structures
// ============================================================
typedef struct _DDCOLORKEY {
    DWORD dwColorSpaceLowValue;
    DWORD dwColorSpaceHighValue;
} DDCOLORKEY;

typedef struct _DDPIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    union {
        DWORD dwRGBBitCount;
        DWORD dwYUVBitCount;
        DWORD dwZBufferBitDepth;
        DWORD dwAlphaBitDepth;
    };
    union {
        DWORD dwRBitMask;
        DWORD dwYBitMask;
    };
    union {
        DWORD dwGBitMask;
        DWORD dwUBitMask;
    };
    union {
        DWORD dwBBitMask;
        DWORD dwVBitMask;
    };
    union {
        DWORD dwRGBAlphaBitMask;
        DWORD dwYUVAlphaBitMask;
    };
} DDPIXELFORMAT;

typedef struct _DDSCAPS {
    DWORD dwCaps;
} DDSCAPS;

typedef struct _DDSURFACEDESC {
    DWORD               dwSize;
    DWORD               dwFlags;
    DWORD               dwHeight;
    DWORD               dwWidth;
    union {
        LONG            lPitch;
        DWORD           dwLinearSize;
    };
    DWORD               dwBackBufferCount;
    union {
        DWORD           dwMipMapCount;
        DWORD           dwZBufferBitDepth;
        DWORD           dwRefreshRate;
    };
    DWORD               dwAlphaBitDepth;
    DWORD               dwReserved;
    LPVOID              lpSurface;
    DDCOLORKEY          ddckCKDestOverlay;
    DDCOLORKEY          ddckCKDestBlt;
    DDCOLORKEY          ddckCKSrcOverlay;
    DDCOLORKEY          ddckCKSrcBlt;
    DDPIXELFORMAT       ddpfPixelFormat;
    DDSCAPS             ddsCaps;
} DDSURFACEDESC;

typedef struct _DDBLTFX {
    DWORD               dwSize;
    DWORD               dwDDFX;
    DWORD               dwROP;
    DWORD               dwDDROP;
    DWORD               dwRotationAngle;
    DWORD               dwZBufferOpCode;
    DWORD               dwZBufferLow;
    DWORD               dwZBufferHigh;
    DWORD               dwZBufferBaseDest;
    DWORD               dwZDestConstBitDepth;
    union {
        DWORD           dwZDestConst;
        LPDIRECTDRAWSURFACE lpDDSZBufferDest;
    };
    DWORD               dwZSrcConstBitDepth;
    union {
        DWORD           dwZSrcConst;
        LPDIRECTDRAWSURFACE lpDDSZBufferSrc;
    };
    DWORD               dwAlphaEdgeBlendBitDepth;
    DWORD               dwAlphaEdgeBlend;
    DWORD               dwReserved;
    DWORD               dwAlphaDestConstBitDepth;
    union {
        DWORD           dwAlphaDestConst;
        LPDIRECTDRAWSURFACE lpDDSAlphaDest;
    };
    DWORD               dwAlphaSrcConstBitDepth;
    union {
        DWORD           dwAlphaSrcConst;
        LPDIRECTDRAWSURFACE lpDDSAlphaSrc;
    };
    union {
        DWORD           dwFillColor;
        DWORD           dwFillDepth;
        LPDIRECTDRAWSURFACE lpDDSPattern;
    };
    DDCOLORKEY          ddckDestColorkey;
    DDCOLORKEY          ddckSrcColorkey;
} DDBLTFX;

// SOURCEPORT: LP* typedefs must be before COM interfaces that reference them
typedef DDSURFACEDESC*      LPDDSURFACEDESC;
typedef DDPIXELFORMAT*      LPDDPIXELFORMAT;
typedef DDSCAPS*            LPDDSCAPS;
typedef DDCOLORKEY*         LPDDCOLORKEY;
typedef DDBLTFX*            LPDDBLTFX;

// ============================================================
// COM interfaces (IUnknown-derived stubs with vtables)
// ============================================================

#undef INTERFACE
#define INTERFACE IDirectDrawSurface
DECLARE_INTERFACE_(IDirectDrawSurface, IUnknown)
{
    /*** IUnknown ***/
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    /*** IDirectDrawSurface ***/
    STDMETHOD(AddAttachedSurface)(THIS_ LPDIRECTDRAWSURFACE lpDDSAttached) PURE;
    STDMETHOD(AddOverlayDirtyRect)(THIS_ LPRECT lpRect) PURE;
    STDMETHOD(Blt)(THIS_ LPRECT lpDestRect, LPDIRECTDRAWSURFACE lpDDSrcSurface, LPRECT lpSrcRect, DWORD dwFlags, LPDDBLTFX lpDDBltFx) PURE;
    STDMETHOD(BltBatch)(THIS_ LPVOID lpBatch, DWORD dwCount, DWORD dwFlags) PURE;
    STDMETHOD(BltFast)(THIS_ DWORD dwX, DWORD dwY, LPDIRECTDRAWSURFACE lpSrc, LPRECT lpSrcRect, DWORD dwTrans) PURE;
    STDMETHOD(DeleteAttachedSurface)(THIS_ DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSAttached) PURE;
    STDMETHOD(EnumAttachedSurfaces)(THIS_ LPVOID lpContext, LPVOID lpCallback) PURE;
    STDMETHOD(EnumOverlayZOrders)(THIS_ DWORD dwFlags, LPVOID lpContext, LPVOID lpCallback) PURE;
    STDMETHOD(Flip)(THIS_ LPDIRECTDRAWSURFACE lpDDSurfaceTargetOverride, DWORD dwFlags) PURE;
    STDMETHOD(GetAttachedSurface)(THIS_ LPDDSCAPS lpDDSCaps, LPDIRECTDRAWSURFACE* lplpDDAttachedSurface) PURE;
    STDMETHOD(GetBltStatus)(THIS_ DWORD dwFlags) PURE;
    STDMETHOD(GetCaps)(THIS_ LPDDSCAPS lpDDSCaps) PURE;
    STDMETHOD(GetClipper)(THIS_ LPVOID* lplpDDClipper) PURE;
    STDMETHOD(GetColorKey)(THIS_ DWORD dwFlags, LPDDCOLORKEY lpDDColorKey) PURE;
    STDMETHOD(GetDC)(THIS_ HDC* lphDC) PURE;
    STDMETHOD(GetFlipStatus)(THIS_ DWORD dwFlags) PURE;
    STDMETHOD(GetOverlayPosition)(THIS_ LPLONG lplX, LPLONG lplY) PURE;
    STDMETHOD(GetPalette)(THIS_ LPVOID* lplpDDPalette) PURE;
    STDMETHOD(GetPixelFormat)(THIS_ LPDDPIXELFORMAT lpDDPixelFormat) PURE;
    STDMETHOD(GetSurfaceDesc)(THIS_ LPDDSURFACEDESC lpDDSurfaceDesc) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECTDRAW lpDD, LPDDSURFACEDESC lpDDSurfaceDesc) PURE;
    STDMETHOD(IsLost)(THIS) PURE;
    STDMETHOD(Lock)(THIS_ LPRECT lpDestRect, LPDDSURFACEDESC lpDDSurfaceDesc, DWORD dwFlags, HANDLE hEvent) PURE;
    STDMETHOD(ReleaseDC)(THIS_ HDC hDC) PURE;
    STDMETHOD(Restore)(THIS) PURE;
    STDMETHOD(SetClipper)(THIS_ LPVOID lpDDClipper) PURE;
    STDMETHOD(SetColorKey)(THIS_ DWORD dwFlags, LPDDCOLORKEY lpDDColorKey) PURE;
    STDMETHOD(SetOverlayPosition)(THIS_ LONG lX, LONG lY) PURE;
    STDMETHOD(SetPalette)(THIS_ LPVOID lpDDPalette) PURE;
    STDMETHOD(Unlock)(THIS_ LPVOID lpSurfaceData) PURE;
    STDMETHOD(UpdateOverlay)(THIS_ LPRECT lpSrcRect, LPDIRECTDRAWSURFACE lpDDDestSurface, LPRECT lpDestRect, DWORD dwFlags, LPVOID lpDDOverlayFx) PURE;
    STDMETHOD(UpdateOverlayDisplay)(THIS_ DWORD dwFlags) PURE;
    STDMETHOD(UpdateOverlayZOrder)(THIS_ DWORD dwFlags, LPDIRECTDRAWSURFACE lpDDSReference) PURE;
};

// Callback types
typedef HRESULT (WINAPI* LPDDENUMCALLBACKA)(GUID*, LPSTR, LPSTR, LPVOID);
typedef LPDDENUMCALLBACKA LPDDENUMCALLBACK;

#undef INTERFACE
#define INTERFACE IDirectDraw
DECLARE_INTERFACE_(IDirectDraw, IUnknown)
{
    /*** IUnknown ***/
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    /*** IDirectDraw ***/
    STDMETHOD(Compact)(THIS) PURE;
    STDMETHOD(CreateClipper)(THIS_ DWORD dwFlags, LPVOID* lplpDDClipper, IUnknown* pUnkOuter) PURE;
    STDMETHOD(CreatePalette)(THIS_ DWORD dwFlags, LPVOID lpColorTable, LPVOID* lplpDDPalette, IUnknown* pUnkOuter) PURE;
    STDMETHOD(CreateSurface)(THIS_ LPDDSURFACEDESC lpDDSurfaceDesc, LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter) PURE;
    STDMETHOD(DuplicateSurface)(THIS_ LPDIRECTDRAWSURFACE lpDDSurface, LPDIRECTDRAWSURFACE* lplpDup) PURE;
    STDMETHOD(EnumDisplayModes)(THIS_ DWORD dwFlags, LPDDSURFACEDESC lpDDSurfaceDesc, LPVOID lpContext, LPVOID lpEnumModesCallback) PURE;
    STDMETHOD(EnumSurfaces)(THIS_ DWORD dwFlags, LPDDSURFACEDESC lpDDSD, LPVOID lpContext, LPVOID lpEnumSurfacesCallback) PURE;
    STDMETHOD(FlipToGDISurface)(THIS) PURE;
    STDMETHOD(GetCaps)(THIS_ LPVOID lpDDDriverCaps, LPVOID lpDDHELCaps) PURE;
    STDMETHOD(GetDisplayMode)(THIS_ LPDDSURFACEDESC lpDDSurfaceDesc) PURE;
    STDMETHOD(GetFourCCCodes)(THIS_ LPDWORD lpNumCodes, LPDWORD lpCodes) PURE;
    STDMETHOD(GetGDISurface)(THIS_ LPDIRECTDRAWSURFACE* lplpGDIDDSSurface) PURE;
    STDMETHOD(GetMonitorFrequency)(THIS_ LPDWORD lpdwFrequency) PURE;
    STDMETHOD(GetScanLine)(THIS_ LPDWORD lpdwScanLine) PURE;
    STDMETHOD(GetVerticalBlankStatus)(THIS_ LPBOOL lpbIsInVB) PURE;
    STDMETHOD(Initialize)(THIS_ GUID* lpGUID) PURE;
    STDMETHOD(RestoreDisplayMode)(THIS) PURE;
    STDMETHOD(SetCooperativeLevel)(THIS_ HWND hWnd, DWORD dwFlags) PURE;
    STDMETHOD(SetDisplayMode)(THIS_ DWORD dwWidth, DWORD dwHeight, DWORD dwBPP) PURE;
    STDMETHOD(WaitForVerticalBlank)(THIS_ DWORD dwFlags, HANDLE hEvent) PURE;
};

#undef INTERFACE
#define INTERFACE IDirectDraw2
DECLARE_INTERFACE_(IDirectDraw2, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
};

// ============================================================
// API function
// ============================================================
#ifdef __cplusplus
extern "C" {
#endif

HRESULT WINAPI DirectDrawCreate(GUID* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown* pUnkOuter);

#ifdef __cplusplus
}
#endif
