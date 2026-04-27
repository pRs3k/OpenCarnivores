// XR.cpp — OpenXR bring-up (step 3: GL-bound session + session state pump).
//
// Dynamic-load strategy unchanged from step 1: openxr_loader.dll via
// LoadLibrary so the game still runs on machines without a VR runtime.
//
// Roadmap:
//   step 1: loader probe ✔
//   step 2: xrCreateInstance + xrGetSystem + log system name / form factor ✔
//   step 3: XR_KHR_opengl_enable + xrCreateSession on current HDC/HGLRC,
//           xrPollEvent pump driving xrBeginSession / xrEndSession  ← this commit
//   step 4: xrCreateSwapchain per eye, render target plumbing
//   step 5: xrLocateViews per frame → two view/proj matrices
//   step 6: stereo scene-render hook + xrEndFrame compositor submit
//   step 7: xrSyncActions input layer (controllers → KeyboardState feed)

#include "XR.h"
#include "hunt.h"   // PrintLog
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// ─── Minimal OpenXR ABI (inline subset) ─────────────────────────────────────
// Only the types/enums/constants we actually touch. The ABI is spec-frozen
// at 1.0, so hand-declaring is safe and keeps the dep footprint tiny.
#define XRAPI_CALL __stdcall

typedef int32_t  XrResult;
typedef uint64_t XrVersion;
typedef uint32_t XrFlags32;
typedef uint32_t XrBool32;
typedef uint64_t XrSystemId;
typedef int64_t  XrTime;

typedef struct XrInstance_T*  XrInstanceHandle;
typedef struct XrSession_T*   XrSessionHandle;
#define XR_NULL_HANDLE        0
#define XR_NULL_SYSTEM_ID     0

#define XR_SUCCESS                              0
#define XR_EVENT_UNAVAILABLE                    4
#define XR_ERROR_FORM_FACTOR_UNAVAILABLE       -50

#define XR_MAX_APPLICATION_NAME_SIZE           128
#define XR_MAX_ENGINE_NAME_SIZE                128
#define XR_MAX_SYSTEM_NAME_SIZE                256
#define XR_MAX_RESULT_STRING_SIZE              64

#define XR_MAKE_VERSION(maj,min,pat) \
    (((uint64_t)(maj) << 48) | ((uint64_t)(min) << 32) | (uint64_t)(pat))
#define XR_CURRENT_API_VERSION   XR_MAKE_VERSION(1,0,34)

typedef enum XrStructureType {
    XR_TYPE_UNKNOWN                           = 0,
    XR_TYPE_API_LAYER_PROPERTIES              = 1,
    XR_TYPE_EXTENSION_PROPERTIES              = 2,
    XR_TYPE_INSTANCE_CREATE_INFO              = 3,
    XR_TYPE_SYSTEM_GET_INFO                   = 4,
    XR_TYPE_SYSTEM_PROPERTIES                 = 5,
    XR_TYPE_SESSION_CREATE_INFO               = 8,
    XR_TYPE_SESSION_BEGIN_INFO                = 10,
    XR_TYPE_EVENT_DATA_BUFFER                 = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED  = 17,
    XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR = 1000023000,
    XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR  = 1000023003,
} XrStructureType;

typedef enum XrFormFactor {
    XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1,
} XrFormFactor;

typedef enum XrViewConfigurationType {
    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2,
} XrViewConfigurationType;

typedef enum XrSessionState {
    XR_SESSION_STATE_UNKNOWN      = 0,
    XR_SESSION_STATE_IDLE         = 1,
    XR_SESSION_STATE_READY        = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE      = 4,
    XR_SESSION_STATE_FOCUSED      = 5,
    XR_SESSION_STATE_STOPPING     = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING      = 8,
} XrSessionState;

typedef struct XrApplicationInfo {
    char     applicationName[XR_MAX_APPLICATION_NAME_SIZE];
    uint32_t applicationVersion;
    char     engineName[XR_MAX_ENGINE_NAME_SIZE];
    uint32_t engineVersion;
    XrVersion apiVersion;
} XrApplicationInfo;

typedef struct XrInstanceCreateInfo {
    XrStructureType     type;
    const void*         next;
    XrFlags32           createFlags;
    XrApplicationInfo   applicationInfo;
    uint32_t            enabledApiLayerCount;
    const char* const*  enabledApiLayerNames;
    uint32_t            enabledExtensionCount;
    const char* const*  enabledExtensionNames;
} XrInstanceCreateInfo;

typedef struct XrSystemGetInfo {
    XrStructureType type;
    const void*     next;
    XrFormFactor    formFactor;
} XrSystemGetInfo;

typedef struct XrSystemGraphicsProperties {
    uint32_t maxSwapchainImageHeight;
    uint32_t maxSwapchainImageWidth;
    uint32_t maxLayerCount;
} XrSystemGraphicsProperties;

typedef struct XrSystemTrackingProperties {
    XrBool32 orientationTracking;
    XrBool32 positionTracking;
} XrSystemTrackingProperties;

typedef struct XrSystemProperties {
    XrStructureType            type;
    void*                      next;
    XrSystemId                 systemId;
    uint32_t                   vendorId;
    char                       systemName[XR_MAX_SYSTEM_NAME_SIZE];
    XrSystemGraphicsProperties graphicsProperties;
    XrSystemTrackingProperties trackingProperties;
} XrSystemProperties;

// XR_KHR_opengl_enable
typedef struct XrGraphicsBindingOpenGLWin32KHR {
    XrStructureType type;
    const void*     next;
    HDC             hDC;
    HGLRC           hGLRC;
} XrGraphicsBindingOpenGLWin32KHR;

typedef struct XrGraphicsRequirementsOpenGLKHR {
    XrStructureType type;
    void*           next;
    XrVersion       minApiVersionSupported;
    XrVersion       maxApiVersionSupported;
} XrGraphicsRequirementsOpenGLKHR;

typedef struct XrSessionCreateInfo {
    XrStructureType type;
    const void*     next;
    XrFlags32       createFlags;
    XrSystemId      systemId;
} XrSessionCreateInfo;

typedef struct XrSessionBeginInfo {
    XrStructureType         type;
    const void*             next;
    XrViewConfigurationType primaryViewConfigurationType;
} XrSessionBeginInfo;

// xrPollEvent writes into a 4000-byte buffer then casts based on `type`.
typedef struct XrEventDataBuffer {
    XrStructureType type;
    const void*     next;
    uint8_t         varying[4000];
} XrEventDataBuffer;

typedef struct XrEventDataSessionStateChanged {
    XrStructureType type;
    const void*     next;
    XrSessionHandle session;
    XrSessionState  state;
    XrTime          time;
} XrEventDataSessionStateChanged;

// Function pointer typedefs.
typedef XrResult (XRAPI_CALL *PFN_xrGetInstanceProcAddr)(
    XrInstanceHandle, const char*, void**);
typedef XrResult (XRAPI_CALL *PFN_xrCreateInstance)(
    const XrInstanceCreateInfo*, XrInstanceHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroyInstance)(XrInstanceHandle);
typedef XrResult (XRAPI_CALL *PFN_xrGetSystem)(
    XrInstanceHandle, const XrSystemGetInfo*, XrSystemId*);
typedef XrResult (XRAPI_CALL *PFN_xrGetSystemProperties)(
    XrInstanceHandle, XrSystemId, XrSystemProperties*);
typedef XrResult (XRAPI_CALL *PFN_xrResultToString)(
    XrInstanceHandle, XrResult, char[XR_MAX_RESULT_STRING_SIZE]);
typedef XrResult (XRAPI_CALL *PFN_xrCreateSession)(
    XrInstanceHandle, const XrSessionCreateInfo*, XrSessionHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroySession)(XrSessionHandle);
typedef XrResult (XRAPI_CALL *PFN_xrBeginSession)(
    XrSessionHandle, const XrSessionBeginInfo*);
typedef XrResult (XRAPI_CALL *PFN_xrEndSession)(XrSessionHandle);
typedef XrResult (XRAPI_CALL *PFN_xrRequestExitSession)(XrSessionHandle);
typedef XrResult (XRAPI_CALL *PFN_xrPollEvent)(
    XrInstanceHandle, XrEventDataBuffer*);
typedef XrResult (XRAPI_CALL *PFN_xrGetOpenGLGraphicsRequirementsKHR)(
    XrInstanceHandle, XrSystemId, XrGraphicsRequirementsOpenGLKHR*);

// ─── Module state ────────────────────────────────────────────────────────────
namespace {
    HMODULE                    g_loaderDll         = nullptr;
    PFN_xrGetInstanceProcAddr  g_xrGetIPA          = nullptr;
    PFN_xrCreateInstance       g_xrCreateInstance  = nullptr;
    PFN_xrDestroyInstance      g_xrDestroyInstance = nullptr;
    PFN_xrGetSystem            g_xrGetSystem       = nullptr;
    PFN_xrGetSystemProperties  g_xrGetSystemProps  = nullptr;
    PFN_xrResultToString       g_xrResultToString  = nullptr;
    PFN_xrCreateSession        g_xrCreateSession   = nullptr;
    PFN_xrDestroySession       g_xrDestroySession  = nullptr;
    PFN_xrBeginSession         g_xrBeginSession    = nullptr;
    PFN_xrEndSession           g_xrEndSession      = nullptr;
    PFN_xrRequestExitSession   g_xrRequestExit     = nullptr;
    PFN_xrPollEvent            g_xrPollEvent       = nullptr;
    PFN_xrGetOpenGLGraphicsRequirementsKHR g_xrGetGLReqs = nullptr;

    XrInstanceHandle g_instance     = XR_NULL_HANDLE;
    XrSystemId       g_systemId     = XR_NULL_SYSTEM_ID;
    XrSessionHandle  g_session      = XR_NULL_HANDLE;
    XrSessionState   g_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool             g_sessionBegun = false;   // xrBeginSession called, not yet xrEndSession
    bool             g_available    = false;

    void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

    void LogFmt(const char* fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        Log(buf);
    }

    template <typename FN>
    bool Resolve(XrInstanceHandle instance, const char* name, FN& out) {
        void* p = nullptr;
        XrResult r = g_xrGetIPA(instance, name, &p);
        if (r != XR_SUCCESS || !p) {
            LogFmt("XR: failed to resolve %s (result=%d)\n", name, (int)r);
            out = nullptr;
            return false;
        }
        out = reinterpret_cast<FN>(p);
        return true;
    }

    void LogResult(const char* context, XrResult r) {
        if (g_xrResultToString && g_instance != XR_NULL_HANDLE) {
            char s[XR_MAX_RESULT_STRING_SIZE] = {0};
            g_xrResultToString(g_instance, r, s);
            LogFmt("XR: %s failed: %s (%d)\n", context, s, (int)r);
        } else {
            LogFmt("XR: %s failed with result %d\n", context, (int)r);
        }
    }

    // Create the GL-bound session. Requires a current GL context on this
    // thread; called after Init() + xrGetSystem have succeeded.
    bool CreateSession() {
        if (!g_xrCreateSession) return false;

        HDC   hdc   = wglGetCurrentDC();
        HGLRC hglrc = wglGetCurrentContext();
        if (!hdc || !hglrc) {
            Log("XR: no current GL context at session-create time — skipping.\n");
            return false;
        }

        // Runtime advertises the GL version range it supports. We don't
        // strictly need to switch our context; we just need to not be below
        // minApiVersionSupported. Logged so mismatches are visible.
        if (g_xrGetGLReqs) {
            XrGraphicsRequirementsOpenGLKHR req = {};
            req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR;
            XrResult r = g_xrGetGLReqs(g_instance, g_systemId, &req);
            if (r == XR_SUCCESS) {
                LogFmt("XR: runtime wants GL between %u.%u and %u.%u.\n",
                    (unsigned)((req.minApiVersionSupported >> 48) & 0xFFFF),
                    (unsigned)((req.minApiVersionSupported >> 32) & 0xFFFF),
                    (unsigned)((req.maxApiVersionSupported >> 48) & 0xFFFF),
                    (unsigned)((req.maxApiVersionSupported >> 32) & 0xFFFF));
            }
        }

        XrGraphicsBindingOpenGLWin32KHR bind = {};
        bind.type  = XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR;
        bind.hDC   = hdc;
        bind.hGLRC = hglrc;

        XrSessionCreateInfo sci = {};
        sci.type     = XR_TYPE_SESSION_CREATE_INFO;
        sci.next     = &bind;
        sci.systemId = g_systemId;

        XrResult r = g_xrCreateSession(g_instance, &sci, &g_session);
        if (r != XR_SUCCESS) {
            LogResult("xrCreateSession", r);
            g_session = XR_NULL_HANDLE;
            return false;
        }

        Log("XR: GL-bound session created. Waiting for READY state.\n");
        return true;
    }
}

// ─── Public API ──────────────────────────────────────────────────────────────
namespace XR {

bool Init()
{
    if (g_available) return true;

    g_loaderDll = LoadLibraryA("openxr_loader.dll");
    if (!g_loaderDll) {
        Log("XR: openxr_loader.dll not found — running flat-screen.\n");
        return false;
    }

    g_xrGetIPA = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_loaderDll, "xrGetInstanceProcAddr");
    if (!g_xrGetIPA) {
        Log("XR: loader missing xrGetInstanceProcAddr — unusable.\n");
        FreeLibrary(g_loaderDll); g_loaderDll = nullptr;
        return false;
    }

    if (!Resolve(XR_NULL_HANDLE, "xrCreateInstance", g_xrCreateInstance)) {
        FreeLibrary(g_loaderDll); g_loaderDll = nullptr;
        return false;
    }

    // Step 3: request XR_KHR_opengl_enable so we can attach our GL context
    // to the session. All desktop runtimes (SteamVR, Meta Quest Link, WMR,
    // Monado) implement it. Android/standalone Quest would use
    // XR_KHR_opengl_es_enable instead — a later branch.
    const char* extensions[] = { "XR_KHR_opengl_enable" };

    XrApplicationInfo app = {};
    strncpy(app.applicationName, "OpenCarnivores", XR_MAX_APPLICATION_NAME_SIZE - 1);
    strncpy(app.engineName,      "OpenCarnivores", XR_MAX_ENGINE_NAME_SIZE - 1);
    app.applicationVersion = 1;
    app.engineVersion      = 1;
    app.apiVersion         = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo ci = {};
    ci.type                    = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.applicationInfo         = app;
    ci.enabledExtensionCount   = 1;
    ci.enabledExtensionNames   = extensions;

    XrResult r = g_xrCreateInstance(&ci, &g_instance);
    if (r != XR_SUCCESS) {
        LogFmt("XR: xrCreateInstance failed (%d) — likely no active runtime. Flat-screen only.\n", (int)r);
        FreeLibrary(g_loaderDll); g_loaderDll = nullptr;
        g_xrCreateInstance = nullptr; g_xrGetIPA = nullptr;
        return false;
    }

    Resolve(g_instance, "xrDestroyInstance",     g_xrDestroyInstance);
    Resolve(g_instance, "xrGetSystem",           g_xrGetSystem);
    Resolve(g_instance, "xrGetSystemProperties", g_xrGetSystemProps);
    Resolve(g_instance, "xrResultToString",      g_xrResultToString);
    Resolve(g_instance, "xrCreateSession",       g_xrCreateSession);
    Resolve(g_instance, "xrDestroySession",      g_xrDestroySession);
    Resolve(g_instance, "xrBeginSession",        g_xrBeginSession);
    Resolve(g_instance, "xrEndSession",          g_xrEndSession);
    Resolve(g_instance, "xrRequestExitSession",  g_xrRequestExit);
    Resolve(g_instance, "xrPollEvent",           g_xrPollEvent);
    Resolve(g_instance, "xrGetOpenGLGraphicsRequirementsKHR", g_xrGetGLReqs);

    g_available = true;   // loader + instance ready even if no HMD

    if (g_xrGetSystem) {
        XrSystemGetInfo si = {};
        si.type       = XR_TYPE_SYSTEM_GET_INFO;
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        r = g_xrGetSystem(g_instance, &si, &g_systemId);
        if (r != XR_SUCCESS) {
            if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE)
                Log("XR: runtime present but no HMD connected. Flat-screen.\n");
            else
                LogResult("xrGetSystem", r);
            return false;
        }
    }

    if (g_xrGetSystemProps && g_systemId != XR_NULL_SYSTEM_ID) {
        XrSystemProperties props = {};
        props.type = XR_TYPE_SYSTEM_PROPERTIES;
        r = g_xrGetSystemProps(g_instance, g_systemId, &props);
        if (r == XR_SUCCESS) {
            LogFmt("XR: HMD ready — '%s' (vendor 0x%04X), max swapchain %ux%u, %s tracking.\n",
                props.systemName, (unsigned)props.vendorId,
                (unsigned)props.graphicsProperties.maxSwapchainImageWidth,
                (unsigned)props.graphicsProperties.maxSwapchainImageHeight,
                (props.trackingProperties.positionTracking ? "6DoF" : "3DoF"));
        }
    }

    // Session needs a current GL context. XR::Init() is called right after
    // Gamepad::Init(), before InitEngine/Activate3DHardware, so the context
    // doesn't exist yet. We defer session creation to the first PollEvents()
    // call — by then Activate3DHardware has made the context current.
    return true;
}

void PollEvents()
{
    if (!g_available || !g_xrPollEvent) return;

    // Lazy session create: happens the first time PollEvents runs after
    // the GL context is current. Keeps Init() order independent of render
    // bring-up.
    if (g_session == XR_NULL_HANDLE && g_systemId != XR_NULL_SYSTEM_ID) {
        if (wglGetCurrentContext()) CreateSession();
    }

    for (;;) {
        XrEventDataBuffer ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        XrResult r = g_xrPollEvent(g_instance, &ev);
        if (r == XR_EVENT_UNAVAILABLE) break;
        if (r != XR_SUCCESS) { LogResult("xrPollEvent", r); break; }

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged* sc =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_sessionState = sc->state;
            LogFmt("XR: session state -> %d\n", (int)g_sessionState);

            switch (g_sessionState) {
            case XR_SESSION_STATE_READY:
                if (g_session != XR_NULL_HANDLE && g_xrBeginSession && !g_sessionBegun) {
                    XrSessionBeginInfo bi = {};
                    bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult br = g_xrBeginSession(g_session, &bi);
                    if (br == XR_SUCCESS) {
                        g_sessionBegun = true;
                        Log("XR: session begun (stereo).\n");
                    } else {
                        LogResult("xrBeginSession", br);
                    }
                }
                break;
            case XR_SESSION_STATE_STOPPING:
                if (g_sessionBegun && g_xrEndSession) {
                    g_xrEndSession(g_session);
                    g_sessionBegun = false;
                }
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                // Runtime wants us to tear down. Shutdown() will be called
                // on app exit; step 6 will add a graceful mid-game path.
                break;
            default: break;
            }
        }
        // Other event types (reference-space change, instance loss, etc.)
        // are ignored in step 3; relevant ones come online with step 5/6.
    }
}

void Shutdown()
{
    if (g_session != XR_NULL_HANDLE) {
        if (g_sessionBegun && g_xrEndSession) g_xrEndSession(g_session);
        if (g_xrDestroySession) g_xrDestroySession(g_session);
    }
    g_session      = XR_NULL_HANDLE;
    g_sessionBegun = false;
    g_sessionState = XR_SESSION_STATE_UNKNOWN;

    if (g_instance != XR_NULL_HANDLE && g_xrDestroyInstance) {
        g_xrDestroyInstance(g_instance);
    }
    g_instance = XR_NULL_HANDLE;
    g_systemId = XR_NULL_SYSTEM_ID;

    if (g_loaderDll) { FreeLibrary(g_loaderDll); g_loaderDll = nullptr; }

    g_xrGetIPA = nullptr;
    g_xrCreateInstance = nullptr; g_xrDestroyInstance = nullptr;
    g_xrGetSystem = nullptr; g_xrGetSystemProps = nullptr;
    g_xrResultToString = nullptr;
    g_xrCreateSession = nullptr; g_xrDestroySession = nullptr;
    g_xrBeginSession = nullptr; g_xrEndSession = nullptr;
    g_xrRequestExit = nullptr; g_xrPollEvent = nullptr;
    g_xrGetGLReqs = nullptr;
    g_available = false;
}

bool Available()      { return g_available; }
bool SessionRunning() {
    return g_sessionBegun && (
        g_sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        g_sessionState == XR_SESSION_STATE_VISIBLE      ||
        g_sessionState == XR_SESSION_STATE_FOCUSED);
}

} // namespace XR
