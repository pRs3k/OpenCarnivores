// SOURCEPORT: Stub Direct3D (Immediate Mode, version 3/6 era) header for
// compiling Carnivores 2 with modern MSVC. Provides type definitions and
// COM interface stubs. The D3D6 renderer will be replaced in Phase 3.
#pragma once

#include "ddraw.h"

// ============================================================
// GUIDs — defined in dxguid.lib, just declare extern here
// ============================================================
#ifdef __cplusplus
extern "C" {
#endif
extern const GUID IID_IDirect3D;
extern const GUID IID_IDirect3DTexture;
#ifdef __cplusplus
}
#endif

// ============================================================
// Forward declarations
// ============================================================
struct IDirect3D;
struct IDirect3DDevice;
struct IDirect3DViewport;
struct IDirect3DExecuteBuffer;
struct IDirect3DTexture;

typedef IDirect3D*              LPDIRECT3D;
typedef IDirect3DDevice*        LPDIRECT3DDEVICE;
typedef IDirect3DViewport*      LPDIRECT3DVIEWPORT;
typedef IDirect3DExecuteBuffer* LPDIRECT3DEXECUTEBUFFER;
typedef IDirect3DTexture*       LPDIRECT3DTEXTURE;

// ============================================================
// Basic types
// ============================================================
typedef float   D3DVALUE;
typedef DWORD   D3DCOLOR;
typedef DWORD   D3DTEXTUREHANDLE;

#ifndef D3DVAL
#define D3DVAL(val) ((D3DVALUE)(val))
#endif

#ifndef D3D_OK
#define D3D_OK  S_OK
#endif

// ============================================================
// Render state types
// ============================================================
typedef enum _D3DRENDERSTATETYPE {
    D3DRENDERSTATE_TEXTUREHANDLE        = 1,
    D3DRENDERSTATE_ANTIALIAS            = 2,
    D3DRENDERSTATE_TEXTUREPERSPECTIVE   = 4,
    D3DRENDERSTATE_ZENABLE              = 7,
    D3DRENDERSTATE_FILLMODE             = 8,
    D3DRENDERSTATE_SHADEMODE            = 9,
    D3DRENDERSTATE_TEXTUREMAPBLEND      = 21,
    D3DRENDERSTATE_CULLMODE             = 22,
    D3DRENDERSTATE_ZWRITEENABLE         = 14,
    D3DRENDERSTATE_ZFUNC                = 23,
    D3DRENDERSTATE_DITHERENABLE         = 26,
    D3DRENDERSTATE_BLENDENABLE          = 27,
    D3DRENDERSTATE_FOGENABLE            = 15,
    D3DRENDERSTATE_SPECULARENABLE       = 29,
    D3DRENDERSTATE_FOGCOLOR             = 34,
    D3DRENDERSTATE_SRCBLEND             = 19,
    D3DRENDERSTATE_DESTBLEND            = 20,
    D3DRENDERSTATE_TEXTUREMAG           = 17,
    D3DRENDERSTATE_TEXTUREMIN           = 18,
    D3DRENDERSTATE_STIPPLEDALPHA        = 33,
    D3DRENDERSTATE_COLORKEYENABLE       = 41,
    D3DRENDERSTATE_ALPHABLENDENABLE     = 27,
    D3DRENDERSTATE_ALPHATESTENABLE      = 15,
    D3DRENDERSTATE_ALPHAREF             = 24,
    D3DRENDERSTATE_ALPHAFUNC            = 25,
    D3DRENDERSTATE_WRAPU                = 44,
    D3DRENDERSTATE_WRAPV                = 45,
} D3DRENDERSTATETYPE;

// ============================================================
// Enumerations
// ============================================================
typedef enum _D3DSHADEMODE {
    D3DSHADE_FLAT       = 1,
    D3DSHADE_GOURAUD    = 2,
    D3DSHADE_PHONG      = 3,
} D3DSHADEMODE;

typedef enum _D3DFILLMODE {
    D3DFILL_POINT       = 1,
    D3DFILL_WIREFRAME   = 2,
    D3DFILL_SOLID       = 3,
} D3DFILLMODE;

typedef enum _D3DBLEND {
    D3DBLEND_ZERO           = 1,
    D3DBLEND_ONE            = 2,
    D3DBLEND_SRCALPHA       = 5,
    D3DBLEND_INVSRCALPHA    = 6,
    D3DBLEND_DESTALPHA      = 7,
    D3DBLEND_INVDESTALPHA   = 8,
} D3DBLEND;

typedef enum _D3DTEXTUREBLEND {
    D3DTBLEND_DECAL             = 1,
    D3DTBLEND_MODULATE          = 2,
    D3DTBLEND_DECALALPHA        = 3,
    D3DTBLEND_MODULATEALPHA     = 4,
} D3DTEXTUREBLEND;

typedef enum _D3DTEXTUREFILTER {
    D3DFILTER_NEAREST           = 1,
    D3DFILTER_LINEAR            = 2,
    D3DFILTER_MIPNEAREST        = 3,
    D3DFILTER_MIPLINEAR         = 4,
    D3DFILTER_LINEARMIPNEAREST  = 5,
    D3DFILTER_LINEARMIPLINEAR   = 6,
} D3DTEXTUREFILTER;

typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER        = 1,
    D3DCMP_LESS         = 2,
    D3DCMP_EQUAL        = 3,
    D3DCMP_LESSEQUAL    = 4,
    D3DCMP_GREATER      = 5,
    D3DCMP_NOTEQUAL     = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS       = 8,
} D3DCMPFUNC;

typedef enum _D3DCULL {
    D3DCULL_NONE    = 1,
    D3DCULL_CW      = 2,
    D3DCULL_CCW     = 3,
} D3DCULL;

typedef enum _D3DOPCODE {
    D3DOP_POINT             = 1,
    D3DOP_LINE              = 2,
    D3DOP_TRIANGLE          = 3,
    D3DOP_MATRIXLOAD        = 4,
    D3DOP_MATRIXMULTIPLY    = 5,
    D3DOP_STATETRANSFORM    = 6,
    D3DOP_STATELIGHT        = 7,
    D3DOP_STATERENDER       = 8,
    D3DOP_PROCESSVERTICES   = 25,
    D3DOP_EXIT              = 11,
} D3DOPCODE;

// ============================================================
// Shade caps
// ============================================================
#define D3DPSHADECAPS_COLORGOURAUDRGB   0x00000008

// ============================================================
// Execute buffer flags
// ============================================================
#define D3DDEB_BUFSIZE      0x00000001
#define D3DEXECUTE_UNCLIPPED    0x00000001

// ============================================================
// Process vertices flags
// ============================================================
#define D3DPROCESSVERTICES_COPY             0x00000002
#define D3DPROCESSVERTICES_TRANSFORM        0x00000001
#define D3DPROCESSVERTICES_TRANSFORMLIGHT   0x00000000

// ============================================================
// Enumeration return values
// ============================================================
#define D3DENUMRET_OK       S_OK
#define D3DENUMRET_CANCEL   S_FALSE

// ============================================================
// Structures
// ============================================================
typedef struct _D3DTLVERTEX {
    union { D3DVALUE sx;  float dvSX; };
    union { D3DVALUE sy;  float dvSY; };
    union { D3DVALUE sz;  float dvSZ; };
    union { D3DVALUE rhw; float dvRHW; };
    union { D3DCOLOR color;     D3DCOLOR dcColor; };
    union { D3DCOLOR specular;  D3DCOLOR dcSpecular; };
    union { D3DVALUE tu;  float dvTU; };
    union { D3DVALUE tv;  float dvTV; };
} D3DTLVERTEX, *LPD3DTLVERTEX;

typedef struct _D3DVERTEX {
    union { D3DVALUE x;  float dvX; };
    union { D3DVALUE y;  float dvY; };
    union { D3DVALUE z;  float dvZ; };
    union { D3DVALUE nx; float dvNX; };
    union { D3DVALUE ny; float dvNY; };
    union { D3DVALUE nz; float dvNZ; };
    union { D3DVALUE tu; float dvTU; };
    union { D3DVALUE tv; float dvTV; };
} D3DVERTEX;

typedef struct _D3DTRIANGLE {
    union { WORD v1; WORD wV1; };
    union { WORD v2; WORD wV2; };
    union { WORD v3; WORD wV3; };
    WORD wFlags;
} D3DTRIANGLE, *LPD3DTRIANGLE;

typedef struct _D3DLINE {
    union { WORD v1; WORD wV1; };
    union { WORD v2; WORD wV2; };
} D3DLINE, *LPD3DLINE;

typedef struct _D3DINSTRUCTION {
    BYTE bOpcode;
    BYTE bSize;
    WORD wCount;
} D3DINSTRUCTION, *LPD3DINSTRUCTION;

// SOURCEPORT: Must match real DX6 layout — state type then value, 8 bytes total
typedef struct _D3DSTATE {
    union {
        D3DRENDERSTATETYPE drstRenderStateType;
    };
    union {
        DWORD dwArg[1];
        D3DVALUE dvArg[1];
    };
} D3DSTATE, *LPD3DSTATE;

typedef struct _D3DPROCESSVERTICES {
    DWORD dwFlags;
    WORD  wStart;
    WORD  wDest;
    DWORD dwCount;
    DWORD dwReserved;
} D3DPROCESSVERTICES, *LPD3DPROCESSVERTICES;

// SOURCEPORT: D3DRECT and D3DSTATUS needed for correct D3DEXECUTEDATA layout
typedef struct _D3DRECT {
    union { LONG x1; LONG lX1; };
    union { LONG y1; LONG lY1; };
    union { LONG x2; LONG lX2; };
    union { LONG y2; LONG lY2; };
} D3DRECT, *LPD3DRECT;

typedef struct _D3DSTATUS {
    DWORD   dwFlags;
    DWORD   dwStatus;
    D3DRECT drExtent;
} D3DSTATUS, *LPD3DSTATUS;

// SOURCEPORT: Fixed — was missing dsStatus field at the end
typedef struct _D3DEXECUTEDATA {
    DWORD     dwSize;
    DWORD     dwVertexOffset;
    DWORD     dwVertexCount;
    DWORD     dwInstructionOffset;
    DWORD     dwInstructionLength;
    DWORD     dwHVertexOffset;
    D3DSTATUS dsStatus;
} D3DEXECUTEDATA;

typedef struct _D3DEXECUTEBUFFERDESC {
    DWORD  dwSize;
    DWORD  dwFlags;
    DWORD  dwCaps;
    DWORD  dwBufferSize;
    LPVOID lpData;
} D3DEXECUTEBUFFERDESC;

typedef struct _D3DPRIMCAPS {
    DWORD dwSize;
    DWORD dwMiscCaps;
    DWORD dwRasterCaps;
    DWORD dwZCmpCaps;
    DWORD dwSrcBlendCaps;
    DWORD dwDestBlendCaps;
    DWORD dwAlphaCmpCaps;
    DWORD dwShadeCaps;
    DWORD dwTextureCaps;
    DWORD dwTextureFilterCaps;
    DWORD dwTextureBlendCaps;
    DWORD dwTextureAddressCaps;
    DWORD dwStippleWidth;
    DWORD dwStippleHeight;
} D3DPRIMCAPS;

// SOURCEPORT: D3DTRANSFORMCAPS and D3DLIGHTINGCAPS needed for correct D3DDEVICEDESC layout
typedef struct _D3DTRANSFORMCAPS {
    DWORD dwSize;
    DWORD dwCaps;
} D3DTRANSFORMCAPS;

typedef struct _D3DLIGHTINGCAPS {
    DWORD dwSize;
    DWORD dwCaps;
    DWORD dwLightingModel;
    DWORD dwNumLights;
} D3DLIGHTINGCAPS;

// SOURCEPORT: Full D3DDEVICEDESC matching the real DX6 SDK layout exactly.
// The original stub had fields in wrong order and missing dtcTransformCaps,
// bClipping, dlcLightingCaps, and all DX5/6 extension fields.
// This caused memory corruption when dgVoodoo2 (or any real D3D6 implementation)
// wrote the full struct into our undersized buffer.
typedef struct _D3DDEVICEDESC {
    DWORD              dwSize;
    DWORD              dwFlags;
    DWORD              dcmColorModel;     // D3DCOLORMODEL
    DWORD              dwDevCaps;
    D3DTRANSFORMCAPS   dtcTransformCaps;
    BOOL               bClipping;
    D3DLIGHTINGCAPS    dlcLightingCaps;
    D3DPRIMCAPS        dpcLineCaps;
    D3DPRIMCAPS        dpcTriCaps;
    DWORD              dwDeviceRenderBitDepth;
    DWORD              dwDeviceZBufferBitDepth;
    DWORD              dwMaxBufferSize;
    DWORD              dwMaxVertexCount;
    // DX5 extensions
    DWORD              dwMinTextureWidth;
    DWORD              dwMinTextureHeight;
    DWORD              dwMaxTextureWidth;
    DWORD              dwMaxTextureHeight;
    DWORD              dwMinStippleWidth;
    DWORD              dwMaxStippleWidth;
    DWORD              dwMinStippleHeight;
    DWORD              dwMaxStippleHeight;
    // DX6 extensions
    DWORD              dwMaxTextureRepeat;
    DWORD              dwMaxTextureAspectRatio;
    DWORD              dwMaxAnisotropy;
    D3DVALUE           dvGuardBandLeft;
    D3DVALUE           dvGuardBandTop;
    D3DVALUE           dvGuardBandRight;
    D3DVALUE           dvGuardBandBottom;
    D3DVALUE           dvExtentsAdjust;
    DWORD              dwStencilCaps;
    DWORD              dwFVFCaps;
    DWORD              dwTextureOpCaps;
    WORD               wMaxTextureBlendStages;
    WORD               wMaxSimultaneousTextures;
} D3DDEVICEDESC, *LPD3DDEVICEDESC;

typedef struct _D3DVIEWPORT {
    DWORD       dwSize;
    DWORD       dwX;
    DWORD       dwY;
    DWORD       dwWidth;
    DWORD       dwHeight;
    D3DVALUE    dvScaleX;
    D3DVALUE    dvScaleY;
    D3DVALUE    dvMaxX;
    D3DVALUE    dvMaxY;
    D3DVALUE    dvMinZ;
    D3DVALUE    dvMaxZ;
} D3DVIEWPORT;

// ============================================================
// Callback types
// ============================================================
typedef HRESULT (WINAPI* LPD3DENUMDEVICESCALLBACK)(
    GUID* lpGuid, LPSTR lpDeviceDescription, LPSTR lpDeviceName,
    LPD3DDEVICEDESC lpD3DHWDeviceDesc, LPD3DDEVICEDESC lpD3DHELDeviceDesc,
    LPVOID lpContext);

// ============================================================
// COM interfaces
// ============================================================
#undef INTERFACE
#define INTERFACE IDirect3DTexture
DECLARE_INTERFACE_(IDirect3DTexture, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECT3DDEVICE lpDirect3DDevice, LPDIRECTDRAWSURFACE lpDDS) PURE;
    STDMETHOD(GetHandle)(THIS_ LPDIRECT3DDEVICE lpDirect3DDevice, D3DTEXTUREHANDLE* lphTexture) PURE;
    STDMETHOD(PaletteChanged)(THIS_ DWORD dwStart, DWORD dwCount) PURE;
    STDMETHOD(Load)(THIS_ LPDIRECT3DTEXTURE lpD3DTexture) PURE;
    STDMETHOD(Unload)(THIS) PURE;
};

#undef INTERFACE
#define INTERFACE IDirect3DExecuteBuffer
DECLARE_INTERFACE_(IDirect3DExecuteBuffer, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECT3DDEVICE lpDirect3DDevice, D3DEXECUTEBUFFERDESC* lpDesc) PURE;
    STDMETHOD(Lock)(THIS_ D3DEXECUTEBUFFERDESC* lpDesc) PURE;
    STDMETHOD(Unlock)(THIS) PURE;
    STDMETHOD(SetExecuteData)(THIS_ D3DEXECUTEDATA* lpData) PURE;
    STDMETHOD(GetExecuteData)(THIS_ D3DEXECUTEDATA* lpData) PURE;
    STDMETHOD(Validate)(THIS_ DWORD* lpdwOffset, LPVOID lpCallback, LPVOID lpUserArg, DWORD dwReserved) PURE;
    STDMETHOD(Optimize)(THIS_ DWORD dwDummy) PURE;
};

#undef INTERFACE
#define INTERFACE IDirect3DViewport
DECLARE_INTERFACE_(IDirect3DViewport, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECT3D lpDirect3D) PURE;
    STDMETHOD(GetViewport)(THIS_ D3DVIEWPORT* lpData) PURE;
    STDMETHOD(SetViewport)(THIS_ D3DVIEWPORT* lpData) PURE;
    STDMETHOD(TransformVertices)(THIS_ DWORD dwVertexCount, LPVOID lpData, DWORD dwFlags, LPDWORD lpOffscreen) PURE;
    STDMETHOD(LightElements)(THIS_ DWORD dwElementCount, LPVOID lpData) PURE;
    STDMETHOD(SetBackground)(THIS_ D3DTEXTUREHANDLE hMat) PURE;
    STDMETHOD(GetBackground)(THIS_ D3DTEXTUREHANDLE* lphMat, LPBOOL lpValid) PURE;
    STDMETHOD(SetBackgroundDepth)(THIS_ LPDIRECTDRAWSURFACE lpDDSurface) PURE;
    STDMETHOD(GetBackgroundDepth)(THIS_ LPDIRECTDRAWSURFACE* lplpDDSurface, LPBOOL lpValid) PURE;
    STDMETHOD(Clear)(THIS_ DWORD dwCount, LPVOID lpRects, DWORD dwFlags) PURE;
    STDMETHOD(AddLight)(THIS_ LPVOID lpDirect3DLight) PURE;
    STDMETHOD(DeleteLight)(THIS_ LPVOID lpDirect3DLight) PURE;
    STDMETHOD(NextLight)(THIS_ LPVOID lpDirect3DLight, LPVOID* lplpDirect3DLight, DWORD dwFlags) PURE;
};

#undef INTERFACE
#define INTERFACE IDirect3DDevice
DECLARE_INTERFACE_(IDirect3DDevice, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Initialize)(THIS_ LPDIRECT3D lpd3d, GUID* lpGUID, D3DDEVICEDESC* lpd3ddvdesc) PURE;
    STDMETHOD(GetCaps)(THIS_ D3DDEVICEDESC* lpD3DHWDevDesc, D3DDEVICEDESC* lpD3DHELDevDesc) PURE;
    STDMETHOD(SwapTextureHandles)(THIS_ LPDIRECT3DTEXTURE lpT1, LPDIRECT3DTEXTURE lpT2) PURE;
    STDMETHOD(CreateExecuteBuffer)(THIS_ D3DEXECUTEBUFFERDESC* lpDesc, LPDIRECT3DEXECUTEBUFFER* lplpDirect3DExecuteBuffer, IUnknown* pUnkOuter) PURE;
    STDMETHOD(GetStats)(THIS_ LPVOID lpD3DStats) PURE;
    STDMETHOD(Execute)(THIS_ LPDIRECT3DEXECUTEBUFFER lpDirect3DExecuteBuffer, LPDIRECT3DVIEWPORT lpDirect3DViewport, DWORD dwFlags) PURE;
    STDMETHOD(AddViewport)(THIS_ LPDIRECT3DVIEWPORT lpDirect3DViewport) PURE;
    STDMETHOD(DeleteViewport)(THIS_ LPDIRECT3DVIEWPORT lpDirect3DViewport) PURE;
    STDMETHOD(NextViewport)(THIS_ LPDIRECT3DVIEWPORT lpDirect3DViewport, LPDIRECT3DVIEWPORT* lplpDirect3DViewport, DWORD dwFlags) PURE;
    STDMETHOD(Pick)(THIS_ LPDIRECT3DEXECUTEBUFFER lpDirect3DExecuteBuffer, LPDIRECT3DVIEWPORT lpDirect3DViewport, DWORD dwFlags, LPVOID lpRect) PURE;
    STDMETHOD(GetPickRecords)(THIS_ LPDWORD lpCount, LPVOID lpD3DPickRec) PURE;
    STDMETHOD(EnumTextureFormats)(THIS_ LPVOID lpd3dEnumTextureProc, LPVOID lpArg) PURE;
    STDMETHOD(CreateMatrix)(THIS_ LPVOID lphMatrix) PURE;
    STDMETHOD(SetMatrix)(THIS_ DWORD hMatrix, LPVOID lpD3DMatrix) PURE;
    STDMETHOD(GetMatrix)(THIS_ DWORD hMatrix, LPVOID lpD3DMatrix) PURE;
    STDMETHOD(DeleteMatrix)(THIS_ DWORD hMatrix) PURE;
    STDMETHOD(BeginScene)(THIS) PURE;
    STDMETHOD(EndScene)(THIS) PURE;
    STDMETHOD(GetDirect3D)(THIS_ LPDIRECT3D* lplpDirect3D) PURE;
};

#undef INTERFACE
#define INTERFACE IDirect3D
DECLARE_INTERFACE_(IDirect3D, IUnknown)
{
    STDMETHOD(QueryInterface)(THIS_ REFIID riid, LPVOID* ppvObj) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    STDMETHOD(Initialize)(THIS_ REFIID riid) PURE;
    STDMETHOD(EnumDevices)(THIS_ LPD3DENUMDEVICESCALLBACK lpEnumDevicesCallback, LPVOID lpUserArg) PURE;
    STDMETHOD(CreateLight)(THIS_ LPVOID* lplpDirect3DLight, IUnknown* pUnkOuter) PURE;
    STDMETHOD(CreateMaterial)(THIS_ LPVOID* lplpDirect3DMaterial, IUnknown* pUnkOuter) PURE;
    STDMETHOD(CreateViewport)(THIS_ LPDIRECT3DVIEWPORT* lplpD3DViewport, IUnknown* pUnkOuter) PURE;
    STDMETHOD(FindDevice)(THIS_ LPVOID lpD3DFDS, LPVOID lpD3DFDR) PURE;
};
