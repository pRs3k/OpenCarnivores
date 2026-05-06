// XR.cpp — OpenXR bring-up (step 6: stereo render hook + xrEndFrame submit).
//
// Dynamic-load strategy unchanged from step 1: openxr_loader.dll via
// LoadLibrary so the game still runs on machines without a VR runtime.
//
// Roadmap:
//   step 1: loader probe ✔
//   step 2: xrCreateInstance + xrGetSystem + log system name / form factor ✔
//   step 3: XR_KHR_opengl_enable + xrCreateSession on current HDC/HGLRC,
//           xrPollEvent pump driving xrBeginSession / xrEndSession ✔
//   step 4: xrCreateSwapchain per eye, render target plumbing (FBOs) ✔
//   step 5: xrLocateViews per frame → two view/proj matrices ✔
//   step 6: stereo scene-render hook + xrEndFrame compositor submit ← this commit
//   step 7: xrSyncActions input layer (controllers → KeyboardState feed)

#include "XR.h"
#include "hunt.h"   // PrintLog
#include <glad/gl.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Shared with Gamepad.cpp / Hunt2.cpp — same externs as Gamepad::Sample uses.
extern void ToggleBinocular();
extern void ChangeCall();
extern int  g_sdlMouseDX;
extern int  g_sdlMouseDY;

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
typedef uint64_t XrViewStateFlags;

typedef struct XrInstance_T*  XrInstanceHandle;
typedef struct XrSession_T*   XrSessionHandle;
typedef struct XrSwapchain_T* XrSwapchainHandle;
typedef struct XrSpace_T*     XrSpaceHandle;
#define XR_NULL_HANDLE        0
#define XR_NULL_SYSTEM_ID     0

#define XR_SUCCESS                              0
#define XR_EVENT_UNAVAILABLE                    4
#define XR_ERROR_SESSION_NOT_RUNNING           -8
#define XR_ERROR_FORM_FACTOR_UNAVAILABLE       -50

#define XR_MAX_APPLICATION_NAME_SIZE           128
#define XR_MAX_ENGINE_NAME_SIZE                128
#define XR_MAX_SYSTEM_NAME_SIZE                256
#define XR_MAX_RESULT_STRING_SIZE              64

#define XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT  0x00000001  // spec: XrFlags64 bit 0
#define XR_SWAPCHAIN_USAGE_SAMPLED_BIT           0x00000020  // spec: XrFlags64 bit 5

#define XR_VIEW_STATE_ORIENTATION_VALID_BIT      0x00000001
#define XR_VIEW_STATE_POSITION_VALID_BIT         0x00000002

#define XR_INFINITE_DURATION  ((int64_t)0x7fffffffffffffffLL)

// GL format codes referenced as int64_t in xrCreateSwapchain.
#define XR_GL_RGBA8          ((int64_t)0x8058)
#define XR_GL_RGB10_A2       ((int64_t)0x8059)
#define XR_GL_RGBA16F        ((int64_t)0x881A)
#define XR_GL_SRGB8_ALPHA8   ((int64_t)0x8C43)

#define XR_MAKE_VERSION(maj,min,pat) \
    (((uint64_t)(maj) << 48) | ((uint64_t)(min) << 32) | (uint64_t)(pat))
#define XR_CURRENT_API_VERSION   XR_MAKE_VERSION(1,0,34)

// Values verified against KhronosGroup/OpenXR-SDK include/openxr/openxr.h (main branch).
typedef enum XrStructureType {
    XR_TYPE_UNKNOWN                              = 0,
    XR_TYPE_API_LAYER_PROPERTIES                 = 1,
    XR_TYPE_EXTENSION_PROPERTIES                 = 2,
    XR_TYPE_INSTANCE_CREATE_INFO                 = 3,
    XR_TYPE_SYSTEM_GET_INFO                      = 4,
    XR_TYPE_SYSTEM_PROPERTIES                    = 5,
    XR_TYPE_VIEW_LOCATE_INFO                     = 6,
    XR_TYPE_VIEW                                 = 7,
    XR_TYPE_SESSION_CREATE_INFO                  = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO                = 9,
    XR_TYPE_SESSION_BEGIN_INFO                   = 10,
    XR_TYPE_VIEW_STATE                           = 11,
    XR_TYPE_FRAME_END_INFO                       = 12,
    XR_TYPE_FRAME_WAIT_INFO                      = 33,
    XR_TYPE_EVENT_DATA_BUFFER                    = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED     = 18,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION         = 35,
    XR_TYPE_COMPOSITION_LAYER_QUAD               = 36,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO          = 37,
    XR_TYPE_FRAME_STATE                          = 44,
    XR_TYPE_VIEW_CONFIGURATION_VIEW              = 41,
    XR_TYPE_FRAME_BEGIN_INFO                     = 46,
    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW    = 48,
    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO         = 55,
    XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO            = 56,
    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO         = 57,
    XR_TYPE_ACTIONS_SYNC_INFO                    = 61,
    // Action system — verified against openxr.h 1.1.59
    XR_TYPE_ACTION_STATE_BOOLEAN                 = 23,
    XR_TYPE_ACTION_STATE_FLOAT                   = 24,
    XR_TYPE_ACTION_STATE_VECTOR2F                = 25,
    XR_TYPE_ACTION_STATE_POSE                    = 27,
    XR_TYPE_ACTION_SET_CREATE_INFO               = 28,
    XR_TYPE_ACTION_CREATE_INFO                   = 29,
    XR_TYPE_ACTION_SPACE_CREATE_INFO             = 38,
    XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING= 51,
    XR_TYPE_INTERACTION_PROFILE_STATE            = 53,
    XR_TYPE_ACTION_STATE_GET_INFO                = 58,
    XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUM_INFO   = 62,
    XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO = 63,
    XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO      = 60,
    XR_TYPE_SPACE_LOCATION                       = 42,
    XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR    = 1000023000,
    XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR           = 1000023004,
    XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR     = 1000023005,
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

typedef enum XrReferenceSpaceType {
    XR_REFERENCE_SPACE_TYPE_VIEW  = 1,
    XR_REFERENCE_SPACE_TYPE_LOCAL = 2,
    XR_REFERENCE_SPACE_TYPE_STAGE = 3,
} XrReferenceSpaceType;

typedef enum XrEnvironmentBlendMode {
    XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1,
} XrEnvironmentBlendMode;

// ─── Math primitives ─────────────────────────────────────────────────────────
typedef struct XrQuaternionf { float x, y, z, w; } XrQuaternionf;
typedef struct XrVector3f    { float x, y, z;    } XrVector3f;
typedef struct XrPosef {
    XrQuaternionf orientation;
    XrVector3f    position;
} XrPosef;

typedef struct XrFovf {
    float angleLeft;
    float angleRight;
    float angleUp;
    float angleDown;
} XrFovf;

typedef struct XrVector2f { float x, y; } XrVector2f;

typedef struct XrActionStateVector2f {
    XrStructureType type; void* next;
    XrVector2f currentState;
    XrBool32   changedSinceLastSync;
    XrTime     lastChangeTime;
    XrBool32   isActive;
} XrActionStateVector2f;

// ─── Struct declarations ─────────────────────────────────────────────────────
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

typedef struct XrViewConfigurationView {
    XrStructureType type;
    void*           next;
    uint32_t        recommendedImageRectWidth;
    uint32_t        recommendedImageRectHeight;
    uint32_t        maxImageRectWidth;
    uint32_t        maxImageRectHeight;
    uint32_t        recommendedSwapchainSampleCount;
    uint32_t        maxSwapchainSampleCount;
} XrViewConfigurationView;

typedef struct XrSwapchainCreateInfo {
    XrStructureType type;
    const void*     next;
    uint64_t        createFlags;  // XrSwapchainCreateFlags — XrFlags64 in ABI
    uint64_t        usageFlags;   // XrSwapchainUsageFlags  — XrFlags64 in ABI
    int64_t         format;
    uint32_t        sampleCount;
    uint32_t        width;
    uint32_t        height;
    uint32_t        faceCount;
    uint32_t        arraySize;
    uint32_t        mipCount;
} XrSwapchainCreateInfo;

// Base header — all XrSwapchainImage* structs begin with type+next.
typedef struct XrSwapchainImageBaseHeader {
    XrStructureType type;
    void*           next;
} XrSwapchainImageBaseHeader;

typedef struct XrSwapchainImageOpenGLKHR {
    XrStructureType type;
    void*           next;
    uint32_t        image;   // GL texture name
} XrSwapchainImageOpenGLKHR;

typedef struct XrSwapchainImageAcquireInfo {
    XrStructureType type;
    const void*     next;
} XrSwapchainImageAcquireInfo;

typedef struct XrSwapchainImageWaitInfo {
    XrStructureType type;
    const void*     next;
    int64_t         timeout;
} XrSwapchainImageWaitInfo;

typedef struct XrSwapchainImageReleaseInfo {
    XrStructureType type;
    const void*     next;
} XrSwapchainImageReleaseInfo;

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

// Step 5: reference space + view location
typedef struct XrReferenceSpaceCreateInfo {
    XrStructureType      type;
    const void*          next;
    XrReferenceSpaceType referenceSpaceType;
    XrPosef              poseInReferenceSpace;
} XrReferenceSpaceCreateInfo;

typedef struct XrViewLocateInfo {
    XrStructureType         type;
    const void*             next;
    XrViewConfigurationType viewConfigurationType;
    XrTime                  displayTime;
    XrSpaceHandle           space;
} XrViewLocateInfo;

typedef struct XrViewState {
    XrStructureType  type;
    void*            next;
    XrViewStateFlags viewStateFlags;
} XrViewState;

typedef struct XrView {
    XrStructureType type;
    void*           next;
    XrPosef         pose;
    XrFovf          fov;
} XrView;

// Step 5: frame-loop structs
typedef struct XrFrameWaitInfo {
    XrStructureType type;
    const void*     next;
} XrFrameWaitInfo;

typedef struct XrFrameState {
    XrStructureType type;
    void*           next;
    XrTime          predictedDisplayTime;
    int64_t         predictedDisplayPeriod;
    XrBool32        shouldRender;
} XrFrameState;

typedef struct XrFrameBeginInfo {
    XrStructureType type;
    const void*     next;
} XrFrameBeginInfo;

// ─── Step 6: composition layer types ─────────────────────────────────────────
// NOTE: enum values below are taken from the OpenXR 1.0 spec ABI table.
// Cross-check against <openxr/openxr.h> if linking the SDK directly.
typedef struct XrOffset2Di { int32_t x, y;            } XrOffset2Di;
typedef struct XrExtent2Di { int32_t width, height;   } XrExtent2Di;
typedef struct XrRect2Di   { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;

// Every composition layer struct begins with this header.
typedef struct XrCompositionLayerBaseHeader {
    XrStructureType type;
    const void*     next;
    uint64_t        layerFlags;  // XrCompositionLayerFlags — XrFlags64 in ABI
    XrSpaceHandle   space;
} XrCompositionLayerBaseHeader;

typedef struct XrSwapchainSubImage {
    XrSwapchainHandle swapchain;
    XrRect2Di         imageRect;
    uint32_t          imageArrayIndex;
} XrSwapchainSubImage;

typedef struct XrCompositionLayerProjectionView {
    XrStructureType     type;
    const void*         next;
    XrPosef             pose;
    XrFovf              fov;
    XrSwapchainSubImage subImage;
} XrCompositionLayerProjectionView;

typedef struct XrCompositionLayerProjection {
    XrStructureType                         type;
    const void*                             next;
    uint64_t                                layerFlags;  // XrFlags64 in ABI
    XrSpaceHandle                           space;
    uint32_t                                viewCount;
    const XrCompositionLayerProjectionView* views;
} XrCompositionLayerProjection;

// ─── Menu quad layer types ────────────────────────────────────────────────────
typedef enum XrEyeVisibility { XR_EYE_VISIBILITY_BOTH = 0 } XrEyeVisibility;
typedef struct XrExtent2Df { float width, height; } XrExtent2Df;

typedef struct XrCompositionLayerQuad {
    XrStructureType     type;
    const void*         next;
    uint64_t            layerFlags;
    XrSpaceHandle       space;
    XrEyeVisibility     eyeVisibility;
    XrSwapchainSubImage subImage;
    XrPosef             pose;
    XrExtent2Df         size;
} XrCompositionLayerQuad;

// xrEndFrame receives an array of pointers to XrCompositionLayerBaseHeader.
// Step 5 submitted layerCount=0; step 6 submits a projection layer.
typedef struct XrFrameEndInfo {
    XrStructureType                          type;
    const void*                              next;
    XrTime                                   displayTime;
    XrEnvironmentBlendMode                   environmentBlendMode;
    uint32_t                                 layerCount;
    const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;

// ─── Action system types ─────────────────────────────────────────────────────
#define XR_MAX_ACTION_SET_NAME_SIZE          64
#define XR_MAX_ACTION_NAME_SIZE              64
#define XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE 128
#define XR_MAX_LOCALIZED_ACTION_NAME_SIZE    128
#define XR_NULL_PATH                         ((uint64_t)0)
#define XR_SPACE_LOCATION_ORIENTATION_VALID_BIT 0x1
#define XR_SPACE_LOCATION_POSITION_VALID_BIT    0x2

typedef uint64_t XrPath;
typedef struct XrActionSet_T* XrActionSetHandle;
typedef struct XrAction_T*    XrActionHandle;

typedef enum XrActionType {
    XR_ACTION_TYPE_BOOLEAN_INPUT  = 1,
    XR_ACTION_TYPE_VECTOR2F_INPUT = 3,
    XR_ACTION_TYPE_POSE_INPUT     = 4,
} XrActionType;

typedef struct XrActionSetCreateInfo {
    XrStructureType type; const void* next;
    char            actionSetName[XR_MAX_ACTION_SET_NAME_SIZE];
    char            localizedActionSetName[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
    uint32_t        priority;
} XrActionSetCreateInfo;

typedef struct XrActionCreateInfo {
    XrStructureType type; const void* next;
    char            actionName[XR_MAX_ACTION_NAME_SIZE];
    XrActionType    actionType;
    uint32_t        countSubactionPaths;
    const XrPath*   subactionPaths;
    char            localizedActionName[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
} XrActionCreateInfo;

typedef struct XrActionSuggestedBinding { XrActionHandle action; XrPath binding; }
    XrActionSuggestedBinding;

typedef struct XrInteractionProfileSuggestedBinding {
    XrStructureType type; const void* next;
    XrPath          interactionProfile;
    uint32_t        countSuggestedBindings;
    const XrActionSuggestedBinding* suggestedBindings;
} XrInteractionProfileSuggestedBinding;

typedef struct XrSessionActionSetsAttachInfo {
    XrStructureType type; const void* next;
    uint32_t        countActionSets;
    const XrActionSetHandle* actionSets;
} XrSessionActionSetsAttachInfo;

typedef struct XrActionStateGetInfo {
    XrStructureType type; const void* next;
    XrActionHandle  action;
    XrPath          subactionPath;
} XrActionStateGetInfo;

typedef struct XrActionStateBoolean {
    XrStructureType type; void* next;
    XrBool32 currentState; XrBool32 changedSinceLastSync;
    XrTime lastChangeTime;  XrBool32 isActive;
} XrActionStateBoolean;

typedef struct XrActionStatePose {
    XrStructureType type; void* next;
    XrBool32 isActive;
} XrActionStatePose;

typedef struct XrActionSpaceCreateInfo {
    XrStructureType type; const void* next;
    XrActionHandle  action;
    XrPath          subactionPath;
    XrPosef         poseInActionSpace;
} XrActionSpaceCreateInfo;

typedef struct XrActiveActionSet { XrActionSetHandle actionSet; XrPath subactionPath; }
    XrActiveActionSet;

typedef struct XrSpaceLocation {
    XrStructureType type; void* next;
    uint64_t  locationFlags;   // XrSpaceLocationFlags — spec mandates 64-bit
    XrPosef   pose;
} XrSpaceLocation;

// ─── Function pointer typedefs ───────────────────────────────────────────────
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
typedef XrResult (XRAPI_CALL *PFN_xrEnumerateViewConfigurationViews)(
    XrInstanceHandle, XrSystemId, XrViewConfigurationType,
    uint32_t, uint32_t*, XrViewConfigurationView*);
typedef XrResult (XRAPI_CALL *PFN_xrEnumerateSwapchainFormats)(
    XrSessionHandle, uint32_t, uint32_t*, int64_t*);
typedef XrResult (XRAPI_CALL *PFN_xrCreateSwapchain)(
    XrSessionHandle, const XrSwapchainCreateInfo*, XrSwapchainHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroySwapchain)(XrSwapchainHandle);
typedef XrResult (XRAPI_CALL *PFN_xrEnumerateSwapchainImages)(
    XrSwapchainHandle, uint32_t, uint32_t*, XrSwapchainImageBaseHeader*);
// Step 6 — swapchain acquire/wait/release per eye per frame
typedef XrResult (XRAPI_CALL *PFN_xrAcquireSwapchainImage)(
    XrSwapchainHandle, const XrSwapchainImageAcquireInfo*, uint32_t*);
typedef XrResult (XRAPI_CALL *PFN_xrWaitSwapchainImage)(
    XrSwapchainHandle, const XrSwapchainImageWaitInfo*);
typedef XrResult (XRAPI_CALL *PFN_xrReleaseSwapchainImage)(
    XrSwapchainHandle, const XrSwapchainImageReleaseInfo*);
// Step 5 — reference space + view location + frame loop
typedef XrResult (XRAPI_CALL *PFN_xrCreateReferenceSpace)(
    XrSessionHandle, const XrReferenceSpaceCreateInfo*, XrSpaceHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroySpace)(XrSpaceHandle);
typedef XrResult (XRAPI_CALL *PFN_xrLocateViews)(
    XrSessionHandle, const XrViewLocateInfo*,
    XrViewState*, uint32_t, uint32_t*, XrView*);
typedef XrResult (XRAPI_CALL *PFN_xrWaitFrame)(
    XrSessionHandle, const XrFrameWaitInfo*, XrFrameState*);
typedef XrResult (XRAPI_CALL *PFN_xrBeginFrame)(
    XrSessionHandle, const XrFrameBeginInfo*);
typedef XrResult (XRAPI_CALL *PFN_xrEndFrame)(
    XrSessionHandle, const XrFrameEndInfo*);

typedef struct XrActionsSyncInfo {
    XrStructureType    type;
    const void*        next;
    uint32_t           countActiveActionSets;
    const void*        activeActionSets;  // XrActiveActionSet* — null when count=0
} XrActionsSyncInfo;
typedef XrResult (XRAPI_CALL *PFN_xrSyncActions)(
    XrSessionHandle, const XrActionsSyncInfo*);
typedef XrResult (XRAPI_CALL *PFN_xrStringToPath)(
    XrInstanceHandle, const char*, XrPath*);
typedef XrResult (XRAPI_CALL *PFN_xrCreateActionSet)(
    XrInstanceHandle, const XrActionSetCreateInfo*, XrActionSetHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroyActionSet)(XrActionSetHandle);
typedef XrResult (XRAPI_CALL *PFN_xrCreateAction)(
    XrActionSetHandle, const XrActionCreateInfo*, XrActionHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrDestroyAction)(XrActionHandle);
typedef XrResult (XRAPI_CALL *PFN_xrSuggestInteractionProfileBindings)(
    XrInstanceHandle, const XrInteractionProfileSuggestedBinding*);
typedef XrResult (XRAPI_CALL *PFN_xrAttachSessionActionSets)(
    XrSessionHandle, const XrSessionActionSetsAttachInfo*);
typedef XrResult (XRAPI_CALL *PFN_xrGetActionStateBoolean)(
    XrSessionHandle, const XrActionStateGetInfo*, XrActionStateBoolean*);
typedef XrResult (XRAPI_CALL *PFN_xrGetActionStateVector2f)(
    XrSessionHandle, const XrActionStateGetInfo*, XrActionStateVector2f*);
typedef XrResult (XRAPI_CALL *PFN_xrGetActionStatePose)(
    XrSessionHandle, const XrActionStateGetInfo*, XrActionStatePose*);
typedef XrResult (XRAPI_CALL *PFN_xrCreateActionSpace)(
    XrSessionHandle, const XrActionSpaceCreateInfo*, XrSpaceHandle*);
typedef XrResult (XRAPI_CALL *PFN_xrLocateSpace)(
    XrSpaceHandle, XrSpaceHandle, XrTime, XrSpaceLocation*);

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
    PFN_xrGetOpenGLGraphicsRequirementsKHR g_xrGetGLReqs    = nullptr;
    PFN_xrEnumerateViewConfigurationViews  g_xrEnumVCViews  = nullptr;
    PFN_xrEnumerateSwapchainFormats        g_xrEnumSCFmts   = nullptr;
    PFN_xrCreateSwapchain                  g_xrCreateSC     = nullptr;
    PFN_xrDestroySwapchain                 g_xrDestroySC    = nullptr;
    PFN_xrEnumerateSwapchainImages         g_xrEnumSCImages = nullptr;
    // Step 6
    PFN_xrAcquireSwapchainImage g_xrAcquireSCImage = nullptr;
    PFN_xrWaitSwapchainImage    g_xrWaitSCImage    = nullptr;
    PFN_xrReleaseSwapchainImage g_xrReleaseSCImage = nullptr;

    // Step 5
    PFN_xrCreateReferenceSpace g_xrCreateRefSpace = nullptr;
    PFN_xrDestroySpace         g_xrDestroySpace   = nullptr;
    PFN_xrLocateViews          g_xrLocateViews    = nullptr;
    PFN_xrWaitFrame            g_xrWaitFrame      = nullptr;
    PFN_xrBeginFrame           g_xrBeginFrame     = nullptr;
    PFN_xrEndFrame             g_xrEndFrame       = nullptr;
    PFN_xrSyncActions          g_xrSyncActions    = nullptr;
    // Action system
    PFN_xrStringToPath                       g_xrStringToPath    = nullptr;
    PFN_xrCreateActionSet                    g_xrCreateActionSet = nullptr;
    PFN_xrDestroyActionSet                   g_xrDestroyActionSet= nullptr;
    PFN_xrCreateAction                       g_xrCreateAction    = nullptr;
    PFN_xrDestroyAction                      g_xrDestroyAction   = nullptr;
    PFN_xrSuggestInteractionProfileBindings  g_xrSuggestBindings = nullptr;
    PFN_xrAttachSessionActionSets            g_xrAttachActionSets= nullptr;
    PFN_xrGetActionStateBoolean              g_xrGetActionBool   = nullptr;
    PFN_xrGetActionStateVector2f             g_xrGetActionVec2   = nullptr;
    PFN_xrGetActionStatePose                 g_xrGetActionPose   = nullptr;
    PFN_xrCreateActionSpace                  g_xrCreateActionSpace=nullptr;
    PFN_xrLocateSpace                        g_xrLocateSpace     = nullptr;

    XrInstanceHandle g_instance     = XR_NULL_HANDLE;
    XrSystemId       g_systemId     = XR_NULL_SYSTEM_ID;
    XrSessionHandle  g_session      = XR_NULL_HANDLE;
    XrSessionState   g_sessionState = XR_SESSION_STATE_UNKNOWN;
    bool             g_sessionBegun = false;
    bool             g_available    = false;

    // ── Step 4: per-eye swapchain state ──────────────────────────────────────
    static const uint32_t MAX_SC_IMAGES = 4;   // typical runtimes use 2–3

    uint32_t         g_eyeWidth       = 0;
    uint32_t         g_eyeHeight      = 0;
    XrSwapchainHandle g_swapchain[2]  = {};
    uint32_t         g_scLength[2]    = {};    // actual image count per eye

    // GL texture names written by xrEnumerateSwapchainImages
    XrSwapchainImageOpenGLKHR g_scImages[2][MAX_SC_IMAGES] = {};

    // One FBO per swapchain image per eye; depth renderbuffer shared per eye
    GLuint g_eyeFBO[2][MAX_SC_IMAGES] = {};
    GLuint g_eyeDepthRBO[2]           = {};

    // ── Step 5: reference space + per-frame view data ─────────────────────────
    XrSpaceHandle g_refSpace        = XR_NULL_HANDLE;
    XrTime        g_predictedTime   = 0;
    bool          g_frameBegun      = false;   // xrBeginFrame was called this frame
    XrView        g_views[2]        = {};
    bool          g_viewsValid      = false;

    // ── Step 6: per-eye acquire state + projection views for EndFrame ─────────
    uint32_t g_eyeImageIdx[2]       = {};
    bool     g_eyeImageAcquired[2]  = {};
    XrCompositionLayerProjectionView g_projViews[2] = {};
    bool     g_projViewReady[2]     = {};  // true for each eye whose projView was filled
    bool     g_projViewsBuilt       = false;  // true when BOTH eyes filled this frame

    // ── Menu quad swapchain (world-locked 2D panel) ───────────────────────────
    static const uint32_t MENU_SC_W = 1280;
    static const uint32_t MENU_SC_H = 960;
    XrSwapchainHandle g_menuSwapchain     = XR_NULL_HANDLE;
    uint32_t          g_menuScLength      = 0;
    XrSwapchainImageOpenGLKHR g_menuScImages[MAX_SC_IMAGES] = {};
    GLuint            g_menuFBO[MAX_SC_IMAGES]               = {};
    uint32_t          g_menuImageIdx      = 0;
    bool              g_menuImageAcquired = false;
    bool              g_menuLayerReady    = false;
    bool              g_menuPoseLocked    = false;
    // Default pose: 2 m in front of origin, facing the user (identity = +Z toward user).
    XrPosef           g_menuQuadPose      = {{0.f,0.f,0.f,1.f},{0.f,0.f,-2.f}};

    // ── Controller / action system state ─────────────────────────────────────
    XrActionSetHandle g_menuActionSet  = XR_NULL_HANDLE;
    XrActionHandle    g_aimPoseAction  = XR_NULL_HANDLE;
    XrActionHandle    g_selectAction   = XR_NULL_HANDLE;
    // Gameplay button actions (both-hand where applicable)
    XrActionHandle    g_gripAction            = XR_NULL_HANDLE;
    XrActionHandle    g_thumbstickClickAction = XR_NULL_HANDLE;
    XrActionHandle    g_leftStickAction       = XR_NULL_HANDLE;  // vector2f, left hand
    XrActionHandle    g_rightStickAction      = XR_NULL_HANDLE;  // vector2f, right hand
    XrVector2f        g_leftStick             = {0, 0};
    XrVector2f        g_rightStick            = {0, 0};
    XrActionHandle    g_aAction               = XR_NULL_HANDLE;
    XrActionHandle    g_bAction               = XR_NULL_HANDLE;
    XrActionHandle    g_xAction               = XR_NULL_HANDLE;
    XrActionHandle    g_yAction               = XR_NULL_HANDLE;
    XrPath            g_handPath[2]    = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpaceHandle     g_aimSpace[2]    = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool              g_actionsReady   = false;
    // Current held state for each VR_BTN_* code (1-based; index 0 unused).
    bool              g_vrBtnHeld[VR_BTN_LAST + 1] = {};
    bool              g_vrBtnPrev[VR_BTN_LAST + 1] = {};

    struct CtrlState {
        bool    aimValid;
        XrPosef aimPose;
        bool    selectNow;
        bool    selectPrev;
    };
    CtrlState g_ctrl[2] = {};

    // ── Helpers ───────────────────────────────────────────────────────────────
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

    // Destroy swapchains + their FBOs/RBOs. Idempotent.
    void DestroySwapchains() {
        for (int eye = 0; eye < 2; ++eye) {
            // Delete FBOs
            for (uint32_t i = 0; i < g_scLength[eye]; ++i) {
                if (g_eyeFBO[eye][i]) {
                    glDeleteFramebuffers(1, &g_eyeFBO[eye][i]);
                    g_eyeFBO[eye][i] = 0;
                }
            }
            // Delete depth renderbuffer
            if (g_eyeDepthRBO[eye]) {
                glDeleteRenderbuffers(1, &g_eyeDepthRBO[eye]);
                g_eyeDepthRBO[eye] = 0;
            }
            // Destroy swapchain
            if (g_swapchain[eye] != XR_NULL_HANDLE && g_xrDestroySC) {
                g_xrDestroySC(g_swapchain[eye]);
                g_swapchain[eye] = XR_NULL_HANDLE;
            }
            g_scLength[eye] = 0;
        }
        g_eyeWidth  = 0;
        g_eyeHeight = 0;

        // Destroy menu quad swapchain.
        for (uint32_t i = 0; i < g_menuScLength; ++i) {
            if (g_menuFBO[i]) { glDeleteFramebuffers(1, &g_menuFBO[i]); g_menuFBO[i] = 0; }
        }
        if (g_menuSwapchain != XR_NULL_HANDLE && g_xrDestroySC) {
            g_xrDestroySC(g_menuSwapchain);
            g_menuSwapchain = XR_NULL_HANDLE;
        }
        g_menuScLength      = 0;
        g_menuImageAcquired = false;
        g_menuLayerReady    = false;
        g_menuPoseLocked    = false;
    }

    // Select the best GL swapchain format from the runtime's supported list.
    // Preference order: RGBA8 > RGB10_A2 > RGBA16F > SRGB8_ALPHA8 > first.
    // RGBA8 is always preferred: this game uses RGB555 legacy textures with
    // no linear-light workflow, so sRGB output adds no value and Meta Link
    // rejects GL_SRGB8_ALPHA8 swapchains on OpenGL 3.3 contexts anyway.
    int64_t ChooseSwapchainFormat() {
        if (!g_xrEnumSCFmts || g_session == XR_NULL_HANDLE) return XR_GL_RGBA8;

        uint32_t count = 0;
        g_xrEnumSCFmts(g_session, 0, &count, nullptr);
        if (!count) return XR_GL_RGBA8;

        int64_t fmts[32] = {};
        if (count > 32) count = 32;
        g_xrEnumSCFmts(g_session, count, &count, fmts);

        // Log all supported formats to aid future debugging.
        for (uint32_t i = 0; i < count; ++i)
            LogFmt("XR: supported swapchain format[%u] = 0x%llX\n",
                   i, (unsigned long long)fmts[i]);

        bool hasRGBA8 = false, hasRGB10A2 = false,
             hasRGBA16F = false, hasSRGB = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (fmts[i] == XR_GL_RGBA8)        hasRGBA8   = true;
            if (fmts[i] == XR_GL_RGB10_A2)     hasRGB10A2 = true;
            if (fmts[i] == XR_GL_RGBA16F)      hasRGBA16F = true;
            if (fmts[i] == XR_GL_SRGB8_ALPHA8) hasSRGB    = true;
        }
        if (hasRGBA8)   return XR_GL_RGBA8;
        if (hasRGB10A2) return XR_GL_RGB10_A2;
        if (hasRGBA16F) return XR_GL_RGBA16F;
        if (hasSRGB)    return XR_GL_SRGB8_ALPHA8;
        return fmts[0];
    }

    // Create swapchains for both eyes and build the FBO table.
    // Must be called with a current GL context after CreateSession succeeds.
    bool CreateSwapchains() {
        if (!g_xrEnumVCViews || !g_xrCreateSC || !g_xrEnumSCImages) {
            Log("XR: swapchain functions not resolved — skipping.\n");
            return false;
        }

        // Query recommended per-eye render resolution.
        uint32_t viewCount = 0;
        g_xrEnumVCViews(g_instance, g_systemId,
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        0, &viewCount, nullptr);
        if (viewCount < 2) {
            Log("XR: fewer than 2 stereo views reported — aborting swapchain create.\n");
            return false;
        }

        XrViewConfigurationView vcViews[2] = {};
        vcViews[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        vcViews[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        XrResult r = g_xrEnumVCViews(g_instance, g_systemId,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     2, &viewCount, vcViews);
        if (r != XR_SUCCESS) { LogResult("xrEnumerateViewConfigurationViews", r); return false; }

        // Log all fields so layout bugs are immediately visible in the log.
        // Also log offsetof values to catch any struct-layout mismatch between
        // our hand-declared struct and the runtime's ABI.
        LogFmt("XR: vcView[0]: rec %ux%u  max %ux%u  samples rec%u/max%u\n",
               vcViews[0].recommendedImageRectWidth,
               vcViews[0].recommendedImageRectHeight,
               vcViews[0].maxImageRectWidth,
               vcViews[0].maxImageRectHeight,
               vcViews[0].recommendedSwapchainSampleCount,
               vcViews[0].maxSwapchainSampleCount);
        LogFmt("XR: vcv offsetof: type=%zu next=%zu recW=%zu recH=%zu sizeof=%zu\n",
               offsetof(XrViewConfigurationView, type),
               offsetof(XrViewConfigurationView, next),
               offsetof(XrViewConfigurationView, recommendedImageRectWidth),
               offsetof(XrViewConfigurationView, recommendedImageRectHeight),
               sizeof(XrViewConfigurationView));

        // Use recommended size, but clamp to the per-view maximum so we
        // never request a swapchain larger than the runtime can allocate.
        g_eyeWidth  = vcViews[0].recommendedImageRectWidth;
        if (g_eyeWidth  > vcViews[0].maxImageRectWidth)  g_eyeWidth  = vcViews[0].maxImageRectWidth;
        g_eyeHeight = vcViews[0].recommendedImageRectHeight;
        if (g_eyeHeight > vcViews[0].maxImageRectHeight) g_eyeHeight = vcViews[0].maxImageRectHeight;
        // DIAG: if height > 2*width the runtime likely reported a bad value —
        // clamp to width so we don't create an absurdly tall swapchain.
        if (g_eyeHeight >= g_eyeWidth * 2) {
            LogFmt("XR: eyeHeight %u > 2*eyeWidth %u — clamping to eyeWidth.\n",
                   g_eyeHeight, g_eyeWidth);
            g_eyeHeight = g_eyeWidth;
        }
        LogFmt("XR: per-eye render target %ux%u.\n", g_eyeWidth, g_eyeHeight);

        int64_t format = ChooseSwapchainFormat();
        LogFmt("XR: swapchain format 0x%llX.\n", (unsigned long long)format);

        for (int eye = 0; eye < 2; ++eye) {
            XrSwapchainCreateInfo sci = {};
            sci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
            sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                              XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            sci.format      = format;
            sci.sampleCount = 1;
            sci.width       = g_eyeWidth;
            sci.height      = g_eyeHeight;
            sci.faceCount   = 1;
            sci.arraySize   = 1;
            sci.mipCount    = 1;

            r = g_xrCreateSC(g_session, &sci, &g_swapchain[eye]);
            if (r != XR_SUCCESS) {
                LogResult("xrCreateSwapchain", r);
                DestroySwapchains();
                return false;
            }

            // Enumerate GL textures the runtime allocated for this swapchain.
            uint32_t imgCount = 0;
            g_xrEnumSCImages(g_swapchain[eye], 0, &imgCount, nullptr);
            if (imgCount > MAX_SC_IMAGES) {
                LogFmt("XR: eye %d swapchain has %u images; capping at %u.\n",
                       eye, imgCount, MAX_SC_IMAGES);
                imgCount = MAX_SC_IMAGES;
            }
            g_scLength[eye] = imgCount;

            for (uint32_t i = 0; i < imgCount; ++i) {
                g_scImages[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
                g_scImages[eye][i].next = nullptr;
            }
            r = g_xrEnumSCImages(g_swapchain[eye], imgCount, &imgCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(&g_scImages[eye][0]));
            if (r != XR_SUCCESS) {
                LogResult("xrEnumerateSwapchainImages", r);
                DestroySwapchains();
                return false;
            }

            // The runtime creates swapchain textures with GL's default
            // GL_NEAREST_MIPMAP_LINEAR min filter, but mipCount=1 makes
            // them texture-incomplete for sampling.  Set GL_LINEAR so the
            // compositor can sample from them without a GPU fault.
            for (uint32_t i = 0; i < imgCount; ++i) {
                glBindTexture(GL_TEXTURE_2D, (GLuint)g_scImages[eye][i].image);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glBindTexture(GL_TEXTURE_2D, 0);
            }

            // Create one shared depth renderbuffer for this eye.
            glGenRenderbuffers(1, &g_eyeDepthRBO[eye]);
            glBindRenderbuffer(GL_RENDERBUFFER, g_eyeDepthRBO[eye]);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24,
                                  (GLsizei)g_eyeWidth, (GLsizei)g_eyeHeight);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);

            // Create one FBO per swapchain image, attaching the runtime's
            // texture as colour and the shared depth renderbuffer.
            glGenFramebuffers((GLsizei)imgCount, g_eyeFBO[eye]);
            for (uint32_t i = 0; i < imgCount; ++i) {
                glBindFramebuffer(GL_FRAMEBUFFER, g_eyeFBO[eye][i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                      GL_TEXTURE_2D,
                                      (GLuint)g_scImages[eye][i].image, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                         GL_RENDERBUFFER, g_eyeDepthRBO[eye]);

                GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
                if (status != GL_FRAMEBUFFER_COMPLETE) {
                    LogFmt("XR: eye %d FBO[%u] incomplete (0x%X).\n",
                           eye, i, (unsigned)status);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    DestroySwapchains();
                    return false;
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            LogFmt("XR: eye %d — %u swapchain images, FBOs ready.\n", eye, imgCount);
        }

        // ── Menu quad swapchain ───────────────────────────────────────────────
        {
            XrSwapchainCreateInfo msci = {};
            msci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
            msci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                               XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            msci.format      = format;  // same format as eye swapchains
            msci.sampleCount = 1;
            msci.width       = MENU_SC_W;
            msci.height      = MENU_SC_H;
            msci.faceCount   = 1;
            msci.arraySize   = 1;
            msci.mipCount    = 1;

            XrResult mr = g_xrCreateSC(g_session, &msci, &g_menuSwapchain);
            if (mr != XR_SUCCESS) {
                LogResult("xrCreateSwapchain (menu)", mr);
                // Non-fatal: menus fall back to projection layer.
                g_menuSwapchain = XR_NULL_HANDLE;
            } else {
                uint32_t mc = 0;
                g_xrEnumSCImages(g_menuSwapchain, 0, &mc, nullptr);
                if (mc > MAX_SC_IMAGES) mc = MAX_SC_IMAGES;
                g_menuScLength = mc;
                for (uint32_t i = 0; i < mc; ++i) {
                    g_menuScImages[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
                    g_menuScImages[i].next = nullptr;
                }
                g_xrEnumSCImages(g_menuSwapchain, mc, &mc,
                    reinterpret_cast<XrSwapchainImageBaseHeader*>(&g_menuScImages[0]));
                for (uint32_t i = 0; i < mc; ++i) {
                    glBindTexture(GL_TEXTURE_2D, (GLuint)g_menuScImages[i].image);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
                glGenFramebuffers((GLsizei)mc, g_menuFBO);
                for (uint32_t i = 0; i < mc; ++i) {
                    glBindFramebuffer(GL_FRAMEBUFFER, g_menuFBO[i]);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_TEXTURE_2D,
                                          (GLuint)g_menuScImages[i].image, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                }
                LogFmt("XR: menu quad swapchain %ux%u, %u images.\n",
                       MENU_SC_W, MENU_SC_H, mc);
            }
        }

        return true;
    }

    // Create the stage (or local) reference space for xrLocateViews.
    bool CreateReferenceSpace() {
        if (!g_xrCreateRefSpace || g_session == XR_NULL_HANDLE) return false;

        // Identity pose: no offset from the reference space origin.
        XrPosef identity = {};
        identity.orientation.w = 1.0f;

        // Prefer STAGE (floor-level, world-locked) — fall back to LOCAL (eye-level).
        const XrReferenceSpaceType types[2] = {
            XR_REFERENCE_SPACE_TYPE_STAGE,
            XR_REFERENCE_SPACE_TYPE_LOCAL
        };
        for (int i = 0; i < 2; ++i) {
            XrReferenceSpaceCreateInfo rsci = {};
            rsci.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
            rsci.referenceSpaceType   = types[i];
            rsci.poseInReferenceSpace = identity;
            XrResult r = g_xrCreateRefSpace(g_session, &rsci, &g_refSpace);
            if (r == XR_SUCCESS) {
                LogFmt("XR: reference space type %d created.\n", (int)types[i]);
                return true;
            }
        }
        Log("XR: failed to create any reference space.\n");
        return false;
    }

    void DestroyReferenceSpace() {
        if (g_refSpace != XR_NULL_HANDLE && g_xrDestroySpace) {
            g_xrDestroySpace(g_refSpace);
            g_refSpace = XR_NULL_HANDLE;
        }
    }

    // ── Helper: convert string path ───────────────────────────────────────────
    XrPath StrPath(const char* s) {
        XrPath p = XR_NULL_PATH;
        if (g_xrStringToPath && g_instance != XR_NULL_HANDLE)
            g_xrStringToPath(g_instance, s, &p);
        return p;
    }

    // ── Phase 1: create action set + actions + suggest bindings ──────────────
    // MUST be called before xrCreateSession on Meta PC Link — the runtime
    // rejects xrCreateActionSet if the session already exists.
    // Called from CreateSession(), just before g_xrCreateSession().
    bool SetupActionsPreSession() {
        if (!g_xrCreateActionSet || !g_xrCreateAction || !g_xrSuggestBindings) {
            Log("XR: action-system functions not resolved — skipping pre-session setup.\n");
            return false;
        }

        // Action set
        XrActionSetCreateInfo asci;
        memset(&asci, 0, sizeof(asci));
        asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
        asci.next = nullptr;
        strncpy(asci.actionSetName,          "menu_input", XR_MAX_ACTION_SET_NAME_SIZE-1);
        strncpy(asci.localizedActionSetName, "Menu Input", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE-1);
        asci.priority = 0;
        LogFmt("XR: xrCreateActionSet — instance=%p sizeof(asci)=%zu type=%d name='%s'\n",
            (void*)g_instance, sizeof(asci), (int)asci.type, asci.actionSetName);
        {
            XrResult r = g_xrCreateActionSet(g_instance, &asci, &g_menuActionSet);
            if (r != XR_SUCCESS) { LogResult("xrCreateActionSet", r); return false; }
        }

        // Subaction paths for left/right hand
        g_handPath[0] = StrPath("/user/hand/left");
        g_handPath[1] = StrPath("/user/hand/right");

        // Aim pose action (both hands)
        XrActionCreateInfo aci = {};
        aci.type = XR_TYPE_ACTION_CREATE_INFO;
        strncpy(aci.actionName,          "aim_pose", XR_MAX_ACTION_NAME_SIZE-1);
        strncpy(aci.localizedActionName, "Aim Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
        aci.actionType          = XR_ACTION_TYPE_POSE_INPUT;
        aci.countSubactionPaths = 2;
        aci.subactionPaths      = g_handPath;
        {
            XrResult r = g_xrCreateAction(g_menuActionSet, &aci, &g_aimPoseAction);
            if (r != XR_SUCCESS) { LogResult("xrCreateAction(aim_pose)", r); return false; }
        }

        // Select (trigger) action — boolean (both hands)
        XrActionCreateInfo sci2 = {};
        sci2.type = XR_TYPE_ACTION_CREATE_INFO;
        strncpy(sci2.actionName,          "select", XR_MAX_ACTION_NAME_SIZE-1);
        strncpy(sci2.localizedActionName, "Select", XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
        sci2.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
        sci2.countSubactionPaths = 2;
        sci2.subactionPaths      = g_handPath;
        {
            XrResult r = g_xrCreateAction(g_menuActionSet, &sci2, &g_selectAction);
            if (r != XR_SUCCESS) { LogResult("xrCreateAction(select)", r); return false; }
        }

        // Grip / squeeze — boolean (both hands)
        {
            XrActionCreateInfo ai = {};
            ai.type = XR_TYPE_ACTION_CREATE_INFO;
            strncpy(ai.actionName,          "grip",  XR_MAX_ACTION_NAME_SIZE-1);
            strncpy(ai.localizedActionName, "Grip",  XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
            ai.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
            ai.countSubactionPaths = 2;
            ai.subactionPaths      = g_handPath;
            g_xrCreateAction(g_menuActionSet, &ai, &g_gripAction);
        }
        // Thumbstick click — boolean (both hands)
        {
            XrActionCreateInfo ai = {};
            ai.type = XR_TYPE_ACTION_CREATE_INFO;
            strncpy(ai.actionName,          "thumbstick_click",  XR_MAX_ACTION_NAME_SIZE-1);
            strncpy(ai.localizedActionName, "Thumbstick Click",  XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
            ai.actionType          = XR_ACTION_TYPE_BOOLEAN_INPUT;
            ai.countSubactionPaths = 2;
            ai.subactionPaths      = g_handPath;
            g_xrCreateAction(g_menuActionSet, &ai, &g_thumbstickClickAction);
        }
        // Face buttons — boolean (no subaction paths; each is on a fixed hand)
        auto MakeFaceBtn = [&](const char* name, const char* locName, XrActionHandle& out) {
            XrActionCreateInfo ai = {};
            ai.type = XR_TYPE_ACTION_CREATE_INFO;
            strncpy(ai.actionName,          name,    XR_MAX_ACTION_NAME_SIZE-1);
            strncpy(ai.localizedActionName, locName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
            ai.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            g_xrCreateAction(g_menuActionSet, &ai, &out);
        };
        MakeFaceBtn("btn_a", "A Button", g_aAction);
        MakeFaceBtn("btn_b", "B Button", g_bAction);
        MakeFaceBtn("btn_x", "X Button", g_xAction);
        MakeFaceBtn("btn_y", "Y Button", g_yAction);

        // Thumbstick axes — vector2f, one action per hand (no subaction needed)
        {
            XrActionCreateInfo ai = {};
            ai.type = XR_TYPE_ACTION_CREATE_INFO;
            strncpy(ai.actionName,          "left_stick",   XR_MAX_ACTION_NAME_SIZE-1);
            strncpy(ai.localizedActionName, "Left Stick",   XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
            ai.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
            g_xrCreateAction(g_menuActionSet, &ai, &g_leftStickAction);
        }
        {
            XrActionCreateInfo ai = {};
            ai.type = XR_TYPE_ACTION_CREATE_INFO;
            strncpy(ai.actionName,          "right_stick",  XR_MAX_ACTION_NAME_SIZE-1);
            strncpy(ai.localizedActionName, "Right Stick",  XR_MAX_LOCALIZED_ACTION_NAME_SIZE-1);
            ai.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
            g_xrCreateAction(g_menuActionSet, &ai, &g_rightStickAction);
        }

        // Suggest bindings for every relevant profile.
        // Meta/Oculus Touch (Quest 1/2/3, Touch Pro)
        {
            XrPath profile = StrPath("/interaction_profiles/oculus/touch_controller");
            XrActionSuggestedBinding b[] = {
                {g_aimPoseAction,          StrPath("/user/hand/left/input/aim/pose")},
                {g_aimPoseAction,          StrPath("/user/hand/right/input/aim/pose")},
                {g_selectAction,           StrPath("/user/hand/left/input/trigger/value")},
                {g_selectAction,           StrPath("/user/hand/right/input/trigger/value")},
                {g_gripAction,             StrPath("/user/hand/left/input/squeeze/value")},
                {g_gripAction,             StrPath("/user/hand/right/input/squeeze/value")},
                {g_thumbstickClickAction,  StrPath("/user/hand/left/input/thumbstick/click")},
                {g_thumbstickClickAction,  StrPath("/user/hand/right/input/thumbstick/click")},
                {g_aAction,               StrPath("/user/hand/right/input/a/click")},
                {g_bAction,               StrPath("/user/hand/right/input/b/click")},
                {g_xAction,               StrPath("/user/hand/left/input/x/click")},
                {g_yAction,               StrPath("/user/hand/left/input/y/click")},
                {g_leftStickAction,       StrPath("/user/hand/left/input/thumbstick")},
                {g_rightStickAction,      StrPath("/user/hand/right/input/thumbstick")},
            };
            XrInteractionProfileSuggestedBinding ipb = {};
            ipb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
            ipb.interactionProfile     = profile;
            ipb.countSuggestedBindings = (uint32_t)(sizeof(b)/sizeof(b[0]));
            ipb.suggestedBindings      = b;
            XrResult r = g_xrSuggestBindings(g_instance, &ipb);
            if (r != XR_SUCCESS) LogResult("xrSuggestBindings(oculus/touch)", r);
            else                  Log("XR: bindings suggested for oculus/touch_controller.\n");
        }
        // KHR simple controller (generic fallback — select/click instead of trigger)
        {
            XrPath profile = StrPath("/interaction_profiles/khr/simple_controller");
            XrActionSuggestedBinding b[4] = {
                {g_aimPoseAction, StrPath("/user/hand/left/input/aim/pose")},
                {g_aimPoseAction, StrPath("/user/hand/right/input/aim/pose")},
                {g_selectAction,  StrPath("/user/hand/left/input/select/click")},
                {g_selectAction,  StrPath("/user/hand/right/input/select/click")},
            };
            XrInteractionProfileSuggestedBinding ipb = {};
            ipb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
            ipb.interactionProfile     = profile;
            ipb.countSuggestedBindings = 4;
            ipb.suggestedBindings      = b;
            XrResult r = g_xrSuggestBindings(g_instance, &ipb);
            if (r != XR_SUCCESS) LogResult("xrSuggestBindings(khr/simple)", r);
        }

        Log("XR: pre-session action setup done (set+actions+bindings).\n");
        return true;
    }

    // ── Phase 2: attach + create action spaces ────────────────────────────────
    // Called in the READY state handler, after session creation and before
    // xrBeginSession.  SetupActionsPreSession() must have succeeded first.
    bool SetupActionsPostSession() {
        if (g_menuActionSet == XR_NULL_HANDLE) {
            Log("XR: SetupActionsPostSession skipped — pre-session phase did not run.\n");
            return false;
        }
        if (!g_xrAttachActionSets) {
            Log("XR: xrAttachSessionActionSets not resolved.\n");
            return false;
        }

        // Attach
        XrSessionActionSetsAttachInfo attInfo = {};
        attInfo.type            = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
        attInfo.countActionSets = 1;
        attInfo.actionSets      = &g_menuActionSet;
        {
            XrResult r = g_xrAttachActionSets(g_session, &attInfo);
            if (r != XR_SUCCESS) { LogResult("xrAttachSessionActionSets", r); return false; }
        }

        // Create aim spaces for each hand
        XrPosef identity = {{0,0,0,1},{0,0,0}};
        for (int h = 0; h < 2; ++h) {
            XrActionSpaceCreateInfo aspci = {};
            aspci.type              = XR_TYPE_ACTION_SPACE_CREATE_INFO;
            aspci.action            = g_aimPoseAction;
            aspci.subactionPath     = g_handPath[h];
            aspci.poseInActionSpace = identity;
            if (g_xrCreateActionSpace) {
                XrResult ar = g_xrCreateActionSpace(g_session, &aspci, &g_aimSpace[h]);
                if (ar != XR_SUCCESS) LogResult("xrCreateActionSpace", ar);
                else                  LogFmt("XR: aimSpace[%d] created OK.\n", h);
            }
        }

        g_actionsReady = true;
        Log("XR: controller action system ready (aim + select, both hands).\n");
        return true;
    }

    // Kept for the READY-state call site (now delegates to post-session phase only).
    bool SetupActions() { return SetupActionsPostSession(); }

    // ── Poll controller state (call from BeginFrame) ──────────────────────────
    void PollControllers() {
        if (!g_actionsReady || !g_xrSyncActions || g_session == XR_NULL_HANDLE) {
            static bool s_notReadyLogged = false;
            if (!s_notReadyLogged) {
                s_notReadyLogged = true;
                LogFmt("XR: PollControllers skipped: actionsReady=%d syncFn=%p session=%llu\n",
                       (int)g_actionsReady, (void*)g_xrSyncActions,
                       (unsigned long long)(uintptr_t)g_session);
            }
            return;
        }

        XrActiveActionSet activeSet = {};
        activeSet.actionSet    = g_menuActionSet;
        activeSet.subactionPath = XR_NULL_PATH;
        XrActionsSyncInfo asi = {};
        asi.type                  = XR_TYPE_ACTIONS_SYNC_INFO;
        asi.countActiveActionSets = 1;
        asi.activeActionSets      = &activeSet;
        XrResult r = g_xrSyncActions(g_session, &asi);
        // XR_SESSION_NOT_FOCUSED = 8 (not 7) — silenced input but not an error.
        // Log the first unexpected non-zero result for diagnostics.
        static bool s_syncErrLogged = false;
        if (r != XR_SUCCESS && r != 8) {
            if (!s_syncErrLogged) { s_syncErrLogged = true; LogResult("xrSyncActions", r); }
            return;
        }

        static bool s_ctrlDiagLogged = false;
        for (int h = 0; h < 2; ++h) {
            g_ctrl[h].selectPrev = g_ctrl[h].selectNow;
            g_ctrl[h].aimValid   = false;
            g_ctrl[h].selectNow  = false;

            // Aim pose
            if (g_aimSpace[h] != XR_NULL_HANDLE && g_xrLocateSpace) {
                XrSpaceLocation loc = {};
                loc.type = XR_TYPE_SPACE_LOCATION;
                XrResult lr = g_xrLocateSpace(g_aimSpace[h], g_refSpace, g_predictedTime, &loc);
                if (lr == XR_SUCCESS &&
                    (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                    (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                    g_ctrl[h].aimValid = true;
                    g_ctrl[h].aimPose  = loc.pose;
                } else if (!s_ctrlDiagLogged) {
                    LogFmt("XR: hand[%d] xrLocateSpace r=%d flags=0x%X\n",
                           h, (int)lr, (unsigned)loc.locationFlags);
                }
            } else if (!s_ctrlDiagLogged) {
                LogFmt("XR: hand[%d] aimSpace=%llu xrLocateSpace=%p — not tracking\n",
                       h, (unsigned long long)(uintptr_t)g_aimSpace[h], (void*)g_xrLocateSpace);
            }

            // Trigger / select
            if (g_xrGetActionBool && g_selectAction) {
                XrActionStateGetInfo gi = {};
                gi.type          = XR_TYPE_ACTION_STATE_GET_INFO;
                gi.action        = g_selectAction;
                gi.subactionPath = g_handPath[h];
                XrActionStateBoolean bs = {};
                bs.type = XR_TYPE_ACTION_STATE_BOOLEAN;
                if (g_xrGetActionBool(g_session, &gi, &bs) == XR_SUCCESS && bs.isActive)
                    g_ctrl[h].selectNow = (bs.currentState != 0);
            }
        }
        // Log once when we first achieve valid aim on any hand.
        if (!s_ctrlDiagLogged) {
            if (g_ctrl[0].aimValid || g_ctrl[1].aimValid) {
                s_ctrlDiagLogged = true;
                Log("XR: controller aim tracking active.\n");
            }
        }

        // Mirror trigger state into g_vrBtnHeld[] for SampleVR().
        g_vrBtnHeld[VR_BTN_TRIGGER_R] = g_ctrl[1].selectNow;
        g_vrBtnHeld[VR_BTN_TRIGGER_L] = g_ctrl[0].selectNow;

        // Read per-hand boolean actions: grip and thumbstick click.
        if (g_xrGetActionBool) {
            auto ReadHandBtn = [&](XrActionHandle action, int btnL, int btnR) {
                if (action == XR_NULL_HANDLE) return;
                for (int h = 0; h < 2; ++h) {
                    XrActionStateGetInfo gi = {};
                    gi.type          = XR_TYPE_ACTION_STATE_GET_INFO;
                    gi.action        = action;
                    gi.subactionPath = g_handPath[h];
                    XrActionStateBoolean bs = {};
                    bs.type = XR_TYPE_ACTION_STATE_BOOLEAN;
                    bool held = false;
                    if (g_xrGetActionBool(g_session, &gi, &bs) == XR_SUCCESS && bs.isActive)
                        held = (bs.currentState != 0);
                    g_vrBtnHeld[h == 0 ? btnL : btnR] = held;
                }
            };
            ReadHandBtn(g_gripAction,            VR_BTN_GRIP_L,        VR_BTN_GRIP_R);
            ReadHandBtn(g_thumbstickClickAction, VR_BTN_THUMBSTICK_L,  VR_BTN_THUMBSTICK_R);

            // Face buttons (no subaction path — fixed hand per button).
            auto ReadFaceBtn = [&](XrActionHandle action, int btnIdx) {
                if (action == XR_NULL_HANDLE) return;
                XrActionStateGetInfo gi = {};
                gi.type          = XR_TYPE_ACTION_STATE_GET_INFO;
                gi.action        = action;
                gi.subactionPath = XR_NULL_PATH;
                XrActionStateBoolean bs = {};
                bs.type = XR_TYPE_ACTION_STATE_BOOLEAN;
                bool held = false;
                if (g_xrGetActionBool(g_session, &gi, &bs) == XR_SUCCESS && bs.isActive)
                    held = (bs.currentState != 0);
                g_vrBtnHeld[btnIdx] = held;
            };
            ReadFaceBtn(g_aAction, VR_BTN_A);
            ReadFaceBtn(g_bAction, VR_BTN_B);
            ReadFaceBtn(g_xAction, VR_BTN_X);
            ReadFaceBtn(g_yAction, VR_BTN_Y);
        }

        // Read thumbstick axes (vector2f) and derive virtual direction buttons.
        // OpenXR convention: +X = right, +Y = up (forward push).
        if (g_xrGetActionVec2) {
            auto ReadStick = [&](XrActionHandle action, XrVector2f& out) {
                if (action == XR_NULL_HANDLE) return;
                XrActionStateGetInfo gi = {};
                gi.type          = XR_TYPE_ACTION_STATE_GET_INFO;
                gi.action        = action;
                gi.subactionPath = XR_NULL_PATH;
                XrActionStateVector2f sv = {};
                sv.type = XR_TYPE_ACTION_STATE_VECTOR2F;
                if (g_xrGetActionVec2(g_session, &gi, &sv) == XR_SUCCESS && sv.isActive)
                    out = sv.currentState;
                else
                    out = {0, 0};
            };
            ReadStick(g_leftStickAction,  g_leftStick);
            ReadStick(g_rightStickAction, g_rightStick);

            constexpr float kVBtnThresh = 0.35f;
            // Left stick virtual buttons (+Y = up = forward)
            g_vrBtnHeld[VR_BTN_LS_UP]    = g_leftStick.y  >  kVBtnThresh;
            g_vrBtnHeld[VR_BTN_LS_DOWN]  = g_leftStick.y  < -kVBtnThresh;
            g_vrBtnHeld[VR_BTN_LS_LEFT]  = g_leftStick.x  < -kVBtnThresh;
            g_vrBtnHeld[VR_BTN_LS_RIGHT] = g_leftStick.x  >  kVBtnThresh;
            // Right stick virtual buttons
            g_vrBtnHeld[VR_BTN_RS_UP]    = g_rightStick.y >  kVBtnThresh;
            g_vrBtnHeld[VR_BTN_RS_DOWN]  = g_rightStick.y < -kVBtnThresh;
            g_vrBtnHeld[VR_BTN_RS_LEFT]  = g_rightStick.x < -kVBtnThresh;
            g_vrBtnHeld[VR_BTN_RS_RIGHT] = g_rightStick.x >  kVBtnThresh;
        }
    }

    // Create the GL-bound session. Requires a current GL context on this thread.
    bool CreateSession() {
        if (!g_xrCreateSession) return false;

        HDC   hdc   = wglGetCurrentDC();
        HGLRC hglrc = wglGetCurrentContext();
        if (!hdc || !hglrc) {
            Log("XR: no current GL context at session-create time — skipping.\n");
            return false;
        }

        // Runtime advertises the GL version range it supports.
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
        // Swapchains and reference space are created in the READY state handler
        // (PollEvents), after xrEnumerateSwapchainFormats is valid and the
        // runtime has settled on the final streaming/render resolution.
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

    Resolve(g_instance, "xrDestroyInstance",                    g_xrDestroyInstance);
    Resolve(g_instance, "xrGetSystem",                          g_xrGetSystem);
    Resolve(g_instance, "xrGetSystemProperties",                g_xrGetSystemProps);
    Resolve(g_instance, "xrResultToString",                     g_xrResultToString);
    Resolve(g_instance, "xrCreateSession",                      g_xrCreateSession);
    Resolve(g_instance, "xrDestroySession",                     g_xrDestroySession);
    Resolve(g_instance, "xrBeginSession",                       g_xrBeginSession);
    Resolve(g_instance, "xrEndSession",                         g_xrEndSession);
    Resolve(g_instance, "xrRequestExitSession",                 g_xrRequestExit);
    Resolve(g_instance, "xrPollEvent",                          g_xrPollEvent);
    Resolve(g_instance, "xrGetOpenGLGraphicsRequirementsKHR",   g_xrGetGLReqs);
    Resolve(g_instance, "xrEnumerateViewConfigurationViews",    g_xrEnumVCViews);
    Resolve(g_instance, "xrEnumerateSwapchainFormats",          g_xrEnumSCFmts);
    Resolve(g_instance, "xrCreateSwapchain",                    g_xrCreateSC);
    Resolve(g_instance, "xrDestroySwapchain",                   g_xrDestroySC);
    Resolve(g_instance, "xrEnumerateSwapchainImages",           g_xrEnumSCImages);
    // Step 5
    Resolve(g_instance, "xrAcquireSwapchainImage",              g_xrAcquireSCImage);
    Resolve(g_instance, "xrWaitSwapchainImage",                 g_xrWaitSCImage);
    Resolve(g_instance, "xrReleaseSwapchainImage",              g_xrReleaseSCImage);
    Resolve(g_instance, "xrCreateReferenceSpace",               g_xrCreateRefSpace);
    Resolve(g_instance, "xrDestroySpace",                       g_xrDestroySpace);
    Resolve(g_instance, "xrLocateViews",                        g_xrLocateViews);
    Resolve(g_instance, "xrWaitFrame",                          g_xrWaitFrame);
    Resolve(g_instance, "xrBeginFrame",                         g_xrBeginFrame);
    Resolve(g_instance, "xrEndFrame",                           g_xrEndFrame);
    Resolve(g_instance, "xrSyncActions",                        g_xrSyncActions);
    // Action system
    Resolve(g_instance, "xrStringToPath",                       g_xrStringToPath);
    Resolve(g_instance, "xrCreateActionSet",                    g_xrCreateActionSet);
    Resolve(g_instance, "xrDestroyActionSet",                   g_xrDestroyActionSet);
    Resolve(g_instance, "xrCreateAction",                       g_xrCreateAction);
    Resolve(g_instance, "xrDestroyAction",                      g_xrDestroyAction);
    Resolve(g_instance, "xrSuggestInteractionProfileBindings",  g_xrSuggestBindings);
    Resolve(g_instance, "xrAttachSessionActionSets",            g_xrAttachActionSets);
    Resolve(g_instance, "xrGetActionStateBoolean",              g_xrGetActionBool);
    Resolve(g_instance, "xrGetActionStateVector2f",             g_xrGetActionVec2);
    Resolve(g_instance, "xrGetActionStatePose",                 g_xrGetActionPose);
    Resolve(g_instance, "xrCreateActionSpace",                  g_xrCreateActionSpace);
    Resolve(g_instance, "xrLocateSpace",                        g_xrLocateSpace);

    g_available = true;

    // Create action set immediately after instance creation — must happen
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

    // Action set must be created after xrGetSystem and before xrCreateSession.
    // Meta PC Link requires the system to be identified before accepting action sets.
    SetupActionsPreSession();

    // Session needs a current GL context; deferred to first PollEvents() call.
    return true;
}

void PollEvents()
{
    if (!g_available || !g_xrPollEvent) return;

    // Lazy session create: happens the first time PollEvents runs after
    // the GL context is current.
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
                    // Create swapchains and reference space now — the session is
                    // about to begin, so xrEnumerateSwapchainFormats and
                    // xrEnumerateViewConfigurationViews return valid data.
                    if (g_swapchain[0] == XR_NULL_HANDLE) CreateSwapchains();
                    if (g_refSpace == XR_NULL_HANDLE)     CreateReferenceSpace();
                    if (!g_actionsReady)                  SetupActions();

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
                // Step 6 will add graceful mid-game teardown.
                break;
            default: break;
            }
        }
    }
}

bool BeginFrame()
{
    // Gate on sessionBegun rather than SessionRunning(): Meta Link requires at
    // least one xrWaitFrame call to advance the session from READY (2) to
    // SYNCHRONIZED (3). The spec says xrWaitFrame is only valid in RUNNING
    // states (3/4/5), but Meta's runtime accepts it in READY and uses it as
    // the signal to advance the state machine. On spec-compliant runtimes that
    // reject the call, xrWaitFrame returns SESSION_NOT_RUNNING and we return
    // false — the flat-screen path runs and we retry next frame.
    if (!g_sessionBegun || !g_xrWaitFrame || !g_xrBeginFrame) return false;
    if (g_session == XR_NULL_HANDLE) return false;

    // xrWaitFrame — blocks briefly until the runtime is ready for the next
    // frame and returns the predicted display time + shouldRender flag.
    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState fst = {};
    fst.type = XR_TYPE_FRAME_STATE;
    XrResult r = g_xrWaitFrame(g_session, &fwi, &fst);
    if (r != XR_SUCCESS) {
        // SESSION_NOT_RUNNING is expected while the state is still READY;
        // don't spam the log for this transient condition.
        if (r != XR_ERROR_SESSION_NOT_RUNNING) LogResult("xrWaitFrame", r);
        return false;
    }
    g_predictedTime = fst.predictedDisplayTime;

    // xrBeginFrame — must be called after xrWaitFrame; enables xrEndFrame.
    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
    r = g_xrBeginFrame(g_session, &fbi);
    // XR_FRAME_DISCARDED (9) is a success code meaning the previous frame was
    // discarded; the session is still running and we must continue the loop.
    if (r != XR_SUCCESS && r != 9 /* XR_FRAME_DISCARDED */) {
        LogResult("xrBeginFrame", r); return false;
    }
    if (r != XR_SUCCESS) LogFmt("XR: xrBeginFrame: %d (frame discarded, continuing)\n", (int)r);
    g_frameBegun = true;

    // Reset per-frame layer tracking for the upcoming render.
    g_projViewReady[0] = g_projViewReady[1] = false;
    g_projViewsBuilt   = false;
    g_menuLayerReady   = false;

    // xrLocateViews — per-eye pose + FOV for this predicted display time.
    g_viewsValid = false;
    if (g_xrLocateViews && g_refSpace != XR_NULL_HANDLE && fst.shouldRender) {
        XrViewLocateInfo vli = {};
        vli.type                  = XR_TYPE_VIEW_LOCATE_INFO;
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime           = g_predictedTime;
        vli.space                 = g_refSpace;

        XrViewState vs = {};
        vs.type = XR_TYPE_VIEW_STATE;

        g_views[0].type = XR_TYPE_VIEW;
        g_views[1].type = XR_TYPE_VIEW;
        uint32_t viewCount = 0;
        r = g_xrLocateViews(g_session, &vli, &vs, 2, &viewCount, g_views);
        if (r == XR_SUCCESS && viewCount == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            g_viewsValid = true;
        }
    }

    PollControllers();
    return fst.shouldRender != 0;
}

void EndFrame()
{
    if (!g_frameBegun || !g_xrEndFrame) return;
    g_frameBegun = false;
    g_viewsValid = false;

    XrFrameEndInfo fei = {};
    fei.type                 = XR_TYPE_FRAME_END_INFO;
    fei.displayTime          = g_predictedTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    if (g_projViewsBuilt) {
        // Step 6: submit the stereo projection layer.
        g_projViewsBuilt   = false;
        g_projViewReady[0] = g_projViewReady[1] = false;
        XrCompositionLayerProjection layer = {};
        layer.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.space     = g_refSpace;
        layer.viewCount = 2;
        layer.views     = g_projViews;

        const XrCompositionLayerBaseHeader* pLayers[1] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
        };
        fei.layerCount = 1;
        fei.layers     = pLayers;
    } else if (g_menuLayerReady && g_menuSwapchain != XR_NULL_HANDLE) {
        // Menu mode: submit a world-locked quad layer.
        // The same 2D image is displayed to both eyes from a fixed world pose,
        // so there is no stereo disparity and the menu doesn't move with the head.
        g_menuLayerReady = false;
        XrCompositionLayerQuad quad = {};
        quad.type             = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.space            = g_refSpace;
        quad.eyeVisibility    = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain             = g_menuSwapchain;
        quad.subImage.imageArrayIndex       = 0;
        quad.subImage.imageRect.offset.x    = 0;
        quad.subImage.imageRect.offset.y    = 0;
        quad.subImage.imageRect.extent.width  = (int32_t)MENU_SC_W;
        quad.subImage.imageRect.extent.height = (int32_t)MENU_SC_H;
        quad.pose             = g_menuQuadPose;
        // Physical size: 2.0 m wide × 1.5 m tall (4:3 at a comfortable scale).
        quad.size             = {2.0f, 1.5f};

        const XrCompositionLayerBaseHeader* layers[1] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)
        };
        fei.layerCount = 1;
        fei.layers     = layers;
    } else {
        // No stereo render this frame — present nothing (keeps session alive).
        fei.layerCount = 0;
        fei.layers     = nullptr;
    }

    // xrSyncActions is required before xrEndFrame when the session is FOCUSED,
    // even if no action sets have been attached yet.
    // DIAG: temporarily disabled to test if xrSyncActions is causing xrEndFrame
    // to return XR_ERROR_HANDLE_INVALID on the first FOCUSED frame.
    // if (g_xrSyncActions) {
    //     XrActionsSyncInfo asi = {};
    //     asi.type                  = XR_TYPE_ACTIONS_SYNC_INFO;
    //     asi.countActiveActionSets = 0;
    //     asi.activeActionSets      = nullptr;
    //     XrResult sr = g_xrSyncActions(g_session, &asi);
    //     LogFmt("EF: xrSyncActions returned %d\n", (int)sr);
    // }

    // Reset GL to a clean baseline so the Meta compositor's own GL rendering
    // (lens distortion, timewarp) doesn't inherit our unusual state:
    //   - our shader program (BeginFrame leaves m_shaderProgram active)
    //   - our VAO (should already be 0 but be explicit)
    //   - our reversed depth convention (GL_GEQUAL + glClearDepth(0))
    // glFlush ensures eye-FBO rendering is visible to the compositor's thread.
    glUseProgram(0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glFlush();

    XrResult r = (XrResult)-1;
    EXCEPTION_POINTERS* ep = nullptr;
    __try {
        r = g_xrEndFrame(g_session, &fei);
    } __except (ep = GetExceptionInformation(), EXCEPTION_EXECUTE_HANDLER) {
        ULONG_PTR faultAddr = ep ? (ULONG_PTR)ep->ExceptionRecord->ExceptionAddress : 0;
        ULONG_PTR faultInfo = (ep && ep->ExceptionRecord->NumberParameters >= 2)
                              ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        LogFmt("EF: xrEndFrame EXCEPTION code=0x%08X addr=0x%016llX fault=0x%016llX\n",
               (unsigned)GetExceptionCode(),
               (unsigned long long)faultAddr,
               (unsigned long long)faultInfo);
    }
    if (r != XR_SUCCESS && r != (XrResult)-1) LogResult("xrEndFrame", r);
}

void Shutdown()
{
    // End any in-flight frame cleanly before tearing down.
    if (g_frameBegun && g_xrEndFrame) {
        XrFrameEndInfo fei = {};
        fei.type                 = XR_TYPE_FRAME_END_INFO;
        fei.displayTime          = g_predictedTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount           = 0;
        fei.layers               = nullptr;
        g_xrEndFrame(g_session, &fei);
        g_frameBegun     = false;
        g_projViewsBuilt = false;
    }

    DestroyReferenceSpace();
    DestroySwapchains();

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
    g_xrEnumVCViews = nullptr; g_xrEnumSCFmts = nullptr;
    g_xrCreateSC = nullptr; g_xrDestroySC = nullptr;
    g_xrEnumSCImages = nullptr;
    g_xrAcquireSCImage = nullptr; g_xrWaitSCImage = nullptr;
    g_xrReleaseSCImage = nullptr;
    g_xrCreateRefSpace = nullptr; g_xrDestroySpace = nullptr;
    g_xrLocateViews = nullptr;
    g_xrWaitFrame = nullptr; g_xrBeginFrame = nullptr; g_xrEndFrame = nullptr;
    g_predictedTime  = 0;
    g_viewsValid     = false;
    g_projViewsBuilt = false;
    g_projViewReady[0] = g_projViewReady[1] = false;
    g_eyeImageAcquired[0] = g_eyeImageAcquired[1] = false;
    g_available = false;
}

bool Available()      { return g_available; }
bool SessionRunning() {
    return g_sessionBegun && (
        g_sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        g_sessionState == XR_SESSION_STATE_VISIBLE      ||
        g_sessionState == XR_SESSION_STATE_FOCUSED);
}

uint32_t EyeWidth()        { return g_eyeWidth;  }
uint32_t EyeHeight()       { return g_eyeHeight; }
uint32_t SwapchainLength() { return g_scLength[0]; }

unsigned int EyeFBO(int eye, uint32_t imageIndex) {
    if (eye < 0 || eye > 1) return 0;
    if (imageIndex >= g_scLength[eye]) return 0;
    return (unsigned int)g_eyeFBO[eye][imageIndex];
}

bool ViewsValid()   { return g_viewsValid; }
bool StereoActive() { return g_viewsValid && g_session != XR_NULL_HANDLE; }

} // namespace XR — temporarily close so the C-linkage helper is at file scope
// C-linkage helper callable from renderd3d.cpp without including XR.h namespace
bool XR_StereoActiveForLog() { return XR::StereoActive(); }
namespace XR {

// ── Step 6: per-eye swapchain acquire/release + camera setup ─────────────────

unsigned int AcquireEyeImage(int eye)
{
    if (eye < 0 || eye > 1) return 0;
    if (!g_xrAcquireSCImage || !g_xrWaitSCImage) return 0;
    if (g_swapchain[eye] == XR_NULL_HANDLE) return 0;

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XrResult r = g_xrAcquireSCImage(g_swapchain[eye], &ai, &g_eyeImageIdx[eye]);
    if (r != XR_SUCCESS) { LogResult("xrAcquireSwapchainImage", r); return 0; }

    XrSwapchainImageWaitInfo wi = {};
    wi.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    r = g_xrWaitSCImage(g_swapchain[eye], &wi);
    if (r != XR_SUCCESS) { LogResult("xrWaitSwapchainImage", r); return 0; }

    g_eyeImageAcquired[eye] = true;
    return (unsigned int)g_eyeFBO[eye][g_eyeImageIdx[eye]];
}

void ReleaseEyeImage(int eye, bool symmetricFov)
{
    if (eye < 0 || eye > 1) return;
    if (!g_eyeImageAcquired[eye]) return;
    g_eyeImageAcquired[eye] = false;

    // Record this eye's projection view for EndFrame.
    g_projViews[eye] = {};
    g_projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    g_projViews[eye].pose = g_views[eye].pose;

    if (symmetricFov) {
        // 2D menu blit: both eyes receive identical content.
        // Use the HMD centre pose (midpoint of both eye positions) so the
        // compositor places the image at the same world position for each eye
        // — no IPD parallax.  Also submit symmetric FOV so the image maps
        // identically (left↔right) for both eyes.
        XrPosef centerPose = g_views[eye].pose;
        if (g_viewsValid) {
            centerPose.position.x = (g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f;
            centerPose.position.y = (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f;
            centerPose.position.z = (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f;
            // Use left-eye orientation as the common rotation (both eyes share the same orientation).
            centerPose.orientation = g_views[0].pose.orientation;
        }
        g_projViews[eye].pose = centerPose;

        float hHalf = fmaxf(fabsf(g_views[0].fov.angleLeft),
                            fmaxf(fabsf(g_views[0].fov.angleRight),
                            fmaxf(fabsf(g_views[1].fov.angleLeft),
                                  fabsf(g_views[1].fov.angleRight))));
        float vHalf = fmaxf(fabsf(g_views[0].fov.angleUp),
                            fmaxf(fabsf(g_views[0].fov.angleDown),
                            fmaxf(fabsf(g_views[1].fov.angleUp),
                                  fabsf(g_views[1].fov.angleDown))));
        if (hHalf < 0.01f) hHalf = 0.7854f;
        if (vHalf < 0.01f) vHalf = 0.7854f;
        g_projViews[eye].fov.angleLeft  = -hHalf;
        g_projViews[eye].fov.angleRight =  hHalf;
        g_projViews[eye].fov.angleUp    =  vHalf;
        g_projViews[eye].fov.angleDown  = -vHalf;
    } else {
        // SOURCEPORT: Submit the actual per-eye asymmetric FOV.
        // GetEyeCameraSetup() renders with a matching asymmetric projection
        // (shifted principal point), so the compositor and renderer agree on
        // the angle each pixel represents.
        g_projViews[eye].fov = g_views[eye].fov;
    }
    g_projViews[eye].subImage.swapchain             = g_swapchain[eye];
    g_projViews[eye].subImage.imageArrayIndex       = 0;
    g_projViews[eye].subImage.imageRect.offset.x    = 0;
    g_projViews[eye].subImage.imageRect.offset.y    = 0;
    g_projViews[eye].subImage.imageRect.extent.width  = (int32_t)g_eyeWidth;
    g_projViews[eye].subImage.imageRect.extent.height = (int32_t)g_eyeHeight;

    g_projViewReady[eye] = true;
    // Only mark ready when BOTH eyes have been released this frame.
    // If either eye's AcquireEyeImage failed (returned 0 and was skipped by
    // the caller), its g_projViewReady entry stays false and we submit no
    // layer rather than sending garbage for the missing eye.
    if (g_projViewReady[0] && g_projViewReady[1]) g_projViewsBuilt = true;

    if (g_xrReleaseSCImage && g_swapchain[eye] != XR_NULL_HANDLE) {
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        g_xrReleaseSCImage(g_swapchain[eye], &ri);
    }
}

void GetEyeCameraSetup(int eye,
    float& alpha, float& beta,
    float& dx,    float& dy,    float& dz,
    float& cw,    float& ch,    float& fovk,
    float& vcxFrac, float& vcyFrac)
{
    // Safe defaults if called without valid views.
    if (eye < 0 || eye > 1 || !g_viewsValid) {
        alpha = beta = 0.f; dx = dy = dz = 0.f;
        cw = ch = 400.f; fovk = 1.0f;
        vcxFrac = vcyFrac = 0.5f;
        return;
    }

    // ── Orientation: quaternion → game yaw/pitch ──────────────────────────────
    // OpenXR uses Y-up, -Z forward, X-right (right-handed) — same as the game.
    // Game forward vector: (sin(α)*cos(β), -sin(β), -cos(α)*cos(β))
    // XR forward = R(q) * (0,0,-1) = [ -2(qx*qz+qw*qy),
    //                                    2(qw*qx-qy*qz),
    //                                   2(qx²+qy²)-1    ]
    float qx = g_views[eye].pose.orientation.x;
    float qy = g_views[eye].pose.orientation.y;
    float qz = g_views[eye].pose.orientation.z;
    float qw = g_views[eye].pose.orientation.w;

    float fx = -2.f*(qx*qz + qw*qy);
    float fy =  2.f*(qw*qx - qy*qz);
    float fz =  2.f*(qx*qx + qy*qy) - 1.f;

    alpha = atan2f(fx, -fz);                                    // yaw
    beta  = asinf(fmaxf(-1.f, fminf(1.f, -fy)));               // pitch (-fy: game y = -sin(β))

    // ── Position: IPD offset from HMD center (metres → game units × 256) ──────
    // Use the mid-point between both eye positions as the reference "head centre"
    // so the stereo separation is applied symmetrically around the game pos.
    float cx = (g_views[0].pose.position.x + g_views[1].pose.position.x) * 0.5f;
    float cy = (g_views[0].pose.position.y + g_views[1].pose.position.y) * 0.5f;
    float cz = (g_views[0].pose.position.z + g_views[1].pose.position.z) * 0.5f;

    dx = (g_views[eye].pose.position.x - cx) * 256.f;
    dy = (g_views[eye].pose.position.y - cy) * 256.f;
    dz = (g_views[eye].pose.position.z - cz) * 256.f;
    static int s_ipdLogCount = 0;
    if (s_ipdLogCount < 4) {
        ++s_ipdLogCount;
        LogFmt("IPD eye%d: raw=(%.4f,%.4f,%.4f) ctr=(%.4f,%.4f,%.4f) d=(%.3f,%.3f,%.3f)\n",
               eye,
               g_views[eye].pose.position.x, g_views[eye].pose.position.y, g_views[eye].pose.position.z,
               cx, cy, cz,
               dx, dy, dz);
        LogFmt("FOV eye%d: L=%.3f R=%.3f U=%.3f D=%.3f (deg: L=%.1f R=%.1f U=%.1f D=%.1f)\n",
               eye,
               g_views[eye].fov.angleLeft,  g_views[eye].fov.angleRight,
               g_views[eye].fov.angleUp,    g_views[eye].fov.angleDown,
               g_views[eye].fov.angleLeft  * 57.2958f, g_views[eye].fov.angleRight * 57.2958f,
               g_views[eye].fov.angleUp    * 57.2958f, g_views[eye].fov.angleDown  * 57.2958f);
    }

    // ── Projection: asymmetric FOV from xrLocateViews angles ─────────────────
    // The OpenXR compositor maps eye-image pixels to display angles using the
    // submitted per-eye FOV.  We must render with the same asymmetric projection
    // so the forward direction (x_view=0) lands on the correct off-centre pixel.
    //
    // For Quest 3 left eye (L=-54°, R=+40°):
    //   tL = tan(54°)≈1.376, tR = tan(40°)≈0.839, span = 2.215
    //   focal length cw = eyeWidth / span
    //   principal point = tL / span * eyeWidth ≈ 62.1% from left
    float tL = tanf(fabsf(g_views[eye].fov.angleLeft));   // temporal half-tangent
    float tR = tanf(fabsf(g_views[eye].fov.angleRight));  // nasal  half-tangent
    float tU = tanf(fabsf(g_views[eye].fov.angleUp));
    float tD = tanf(fabsf(g_views[eye].fov.angleDown));
    if (tL < 0.01f) tL = 1.f;   // fallback: 45°
    if (tR < 0.01f) tR = 1.f;
    if (tU < 0.01f) tU = 1.f;
    if (tD < 0.01f) tD = 1.f;

    float eyeW = (float)g_eyeWidth;
    float eyeH = (float)g_eyeHeight;

    // Focal lengths in eye-image pixels.
    cw = eyeW / (tL + tR);
    ch = eyeH / (tU + tD);

    // Principal-point fractions: where forward (0°) hits the eye image.
    vcxFrac = tL / (tL + tR);
    vcyFrac = tU / (tU + tD);

    // fovk is recomputed by the caller once it knows the CPU-space VideoCX.
    // Provide a sentinel; caller must set: FOVK = eyeCW / (VideoCX_new * 1.25f).
    fovk = 0.f;
}

bool GetEyePose(int eye, float pos[3], float orient[4]) {
    if (!g_viewsValid || eye < 0 || eye > 1) return false;
    pos[0]    = g_views[eye].pose.position.x;
    pos[1]    = g_views[eye].pose.position.y;
    pos[2]    = g_views[eye].pose.position.z;
    orient[0] = g_views[eye].pose.orientation.x;
    orient[1] = g_views[eye].pose.orientation.y;
    orient[2] = g_views[eye].pose.orientation.z;
    orient[3] = g_views[eye].pose.orientation.w;
    return true;
}

bool GetEyeFov(int eye, float& left, float& right, float& up, float& down) {
    if (!g_viewsValid || eye < 0 || eye > 1) return false;
    left  = g_views[eye].fov.angleLeft;
    right = g_views[eye].fov.angleRight;
    up    = g_views[eye].fov.angleUp;
    down  = g_views[eye].fov.angleDown;
    return true;
}

int64_t PredictedDisplayTime() { return (int64_t)g_predictedTime; }

bool GetHeadOrientation(float& yaw, float& pitch)
{
    if (!g_viewsValid) return false;

    // Both eyes face the same direction — eye 0's quaternion is the HMD centre
    // orientation.  (The two eye poses only differ in IPD-offset translation.)
    // OpenXR convention: Y-up, -Z forward, X-right (same as the game).
    // Forward vector = R(q) * (0,0,-1):
    //   fx = -2(qx·qz + qw·qy)
    //   fy =  2(qw·qx - qy·qz)
    //   fz =  2(qx²  + qy²) - 1
    float qx = g_views[0].pose.orientation.x;
    float qy = g_views[0].pose.orientation.y;
    float qz = g_views[0].pose.orientation.z;
    float qw = g_views[0].pose.orientation.w;

    float fx = -2.f*(qx*qz + qw*qy);
    float fy =  2.f*(qw*qx - qy*qz);
    float fz =  2.f*(qx*qx + qy*qy) - 1.f;

    yaw   = atan2f(fx, -fz);                                    // game yaw
    pitch = asinf(fmaxf(-1.f, fminf(1.f, -fy)));               // game pitch
    return true;
}

// ── Menu quad swapchain — world-locked 2D panel for menus ────────────────────

uint32_t MenuImageWidth()  { return MENU_SC_W; }
uint32_t MenuImageHeight() { return MENU_SC_H; }

// Call once when entering the menu to place the quad in front of the player.
// Subsequent frames keep the same pose (world-locked, not head-locked).
void ResetMenuQuadPose()
{
    g_menuPoseLocked = false;
}

// Returns the FBO to render/blit the menu into, or 0 on failure.
unsigned int AcquireMenuImage()
{
    if (g_menuSwapchain == XR_NULL_HANDLE) return 0;
    if (!g_xrAcquireSCImage || !g_xrWaitSCImage) return 0;

    // On first call after a ResetMenuQuadPose(), snap the quad position
    // to 2 m in front of the player's current gaze direction.
    if (!g_menuPoseLocked && g_viewsValid) {
        float qx = g_views[0].pose.orientation.x;
        float qy = g_views[0].pose.orientation.y;
        float qz = g_views[0].pose.orientation.z;
        float qw = g_views[0].pose.orientation.w;
        // World-space forward: R(q) * (0,0,-1)
        float fx =  -2.f*(qx*qz + qw*qy);
        float fz =   2.f*(qx*qx + qy*qy) - 1.f;
        float hlen = sqrtf(fx*fx + fz*fz);
        if (hlen > 0.001f) { fx /= hlen; fz /= hlen; }

        float hx = g_views[0].pose.position.x;
        float hy = g_views[0].pose.position.y;
        float hz = g_views[0].pose.position.z;
        g_menuQuadPose.position.x = hx + fx * 2.f;
        g_menuQuadPose.position.y = hy;
        g_menuQuadPose.position.z = hz + fz * 2.f;

        // Orient quad so its +Z faces back toward the player.
        // Rotate default (0,0,1) → (-fx, 0, -fz) via Y-axis rotation θ
        // where sin(θ) = -fx, cos(θ) = -fz.
        float theta = atan2f(-fx, -fz);
        g_menuQuadPose.orientation.x = 0.f;
        g_menuQuadPose.orientation.y = sinf(theta * 0.5f);
        g_menuQuadPose.orientation.z = 0.f;
        g_menuQuadPose.orientation.w = cosf(theta * 0.5f);

        g_menuPoseLocked = true;
    }

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XrResult r = g_xrAcquireSCImage(g_menuSwapchain, &ai, &g_menuImageIdx);
    if (r != XR_SUCCESS) { LogResult("xrAcquireSwapchainImage (menu)", r); return 0; }

    XrSwapchainImageWaitInfo wi = {};
    wi.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    r = g_xrWaitSCImage(g_menuSwapchain, &wi);
    if (r != XR_SUCCESS) { LogResult("xrWaitSwapchainImage (menu)", r); return 0; }

    g_menuImageAcquired = true;
    return (unsigned int)g_menuFBO[g_menuImageIdx];
}

void ReleaseMenuImage()
{
    if (!g_menuImageAcquired) return;
    g_menuImageAcquired = false;

    if (g_xrReleaseSCImage && g_menuSwapchain != XR_NULL_HANDLE) {
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        g_xrReleaseSCImage(g_menuSwapchain, &ri);
    }
    g_menuLayerReady = true;
}

// ── Shared ray-quad intersection ─────────────────────────────────────────────
// Given a ray origin (ox,oy,oz) and normalised forward direction (dx,dy,dz),
// intersects with the world-locked menu quad and fills screenX/Y (game pixel
// coords, Y=0 at top) when the ray hits inside the quad's content region.
// Returns false if the quad isn't locked, the ray is parallel, behind the
// origin, or outside the blitted game-content area.
static bool RayMenuQuad(float ox, float oy, float oz,
                        float dx, float dy, float dz,
                        float& screenX, float& screenY)
{
    if (!g_menuPoseLocked) return false;

    // Quad normal (+Z of quad frame) = R(quadOri) * (0,0,1)
    float nqx = g_menuQuadPose.orientation.x, nqy = g_menuQuadPose.orientation.y;
    float nqz = g_menuQuadPose.orientation.z, nqw = g_menuQuadPose.orientation.w;
    float nx =  2.f*(nqx*nqz + nqw*nqy);
    float ny =  2.f*(nqy*nqz - nqw*nqx);
    float nz =  1.f - 2.f*(nqx*nqx + nqy*nqy);

    float cx = g_menuQuadPose.position.x;
    float cy = g_menuQuadPose.position.y;
    float cz = g_menuQuadPose.position.z;

    float denom = nx*dx + ny*dy + nz*dz;
    if (fabsf(denom) < 0.0001f) return false;
    float t = (nx*(cx-ox) + ny*(cy-oy) + nz*(cz-oz)) / denom;
    if (t < 0.05f) return false;

    float hx = ox + t*dx - cx;
    float hy = oy + t*dy - cy;
    float hz = oz + t*dz - cz;

    // Right = R(quadOri)*(1,0,0), Up = R(quadOri)*(0,1,0)
    float rx = 1.f - 2.f*(nqy*nqy + nqz*nqz);
    float ry = 2.f*(nqx*nqy + nqw*nqz);
    float rz = 2.f*(nqx*nqz - nqw*nqy);
    float ux = 2.f*(nqx*nqy - nqw*nqz);
    float uy = 1.f - 2.f*(nqx*nqx + nqz*nqz);
    float uz = 2.f*(nqy*nqz + nqw*nqx);

    float localU = hx*rx + hy*ry + hz*rz;
    float localV = hx*ux + hy*uy + hz*uz;

    const float QUAD_W = 2.0f, QUAD_H = 1.5f;
    float u = localU / QUAD_W + 0.5f;
    float v = localV / QUAD_H + 0.5f;
    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return false;

    // Account for letterboxing
    float blitH  = (float)WinH * (float)MENU_SC_W / (float)WinW;
    float blitY0 = ((float)MENU_SC_H - blitH) * 0.5f;
    float texY   = v * (float)MENU_SC_H;
    if (texY < blitY0 || texY > blitY0 + blitH) return false;
    float cv = (texY - blitY0) / blitH;

    screenX = u  * (float)WinW;
    screenY = (1.f - cv) * (float)WinH;
    return true;
}

// ── Controller menu cursor ────────────────────────────────────────────────────
bool GetControllerMenuCursor(int hand,
    float& screenX, float& screenY,
    bool& pressed, bool& justPressed, bool& justReleased)
{
    if (hand < 0 || hand > 1) return false;
    if (!g_actionsReady || !g_ctrl[hand].aimValid) return false;

    const XrPosef& aim = g_ctrl[hand].aimPose;
    float ox = aim.position.x, oy = aim.position.y, oz = aim.position.z;
    float qx = aim.orientation.x, qy = aim.orientation.y;
    float qz = aim.orientation.z, qw = aim.orientation.w;
    // Forward = R(q) * (0,0,-1)
    float dx = -2.f*(qx*qz + qw*qy);
    float dy =  2.f*(qw*qx - qy*qz);
    float dz =  2.f*(qx*qx + qy*qy) - 1.f;

    if (!RayMenuQuad(ox, oy, oz, dx, dy, dz, screenX, screenY)) return false;

    pressed      = g_ctrl[hand].selectNow;
    justPressed  = g_ctrl[hand].selectNow  && !g_ctrl[hand].selectPrev;
    justReleased = !g_ctrl[hand].selectNow &&  g_ctrl[hand].selectPrev;
    return true;
}

bool ActionsReady() { return g_actionsReady; }

// ── Head-gaze cursor ──────────────────────────────────────────────────────────
// Uses the left-eye view pose (obtained by BeginFrame/xrLocateViews) as a
// reliable fallback when the controller action system is unavailable.
bool GetHeadGazeCursor(float& screenX, float& screenY)
{
    if (!g_viewsValid) return false;

    const XrPosef& pose = g_views[0].pose;  // left eye ≈ HMD centre
    float ox = pose.position.x, oy = pose.position.y, oz = pose.position.z;
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    // Forward = R(q) * (0,0,-1)
    float dx = -2.f*(qx*qz + qw*qy);
    float dy =  2.f*(qw*qx - qy*qz);
    float dz =  2.f*(qx*qx + qy*qy) - 1.f;

    return RayMenuQuad(ox, oy, oz, dx, dy, dz, screenX, screenY);
}

} // namespace XR (briefly closed so VRMap is at file scope, matching the extern in XR.h)

// ── VR button map — global definition (mirrors KeyMap / PadMap pattern) ──────
TVRMap VRMap = {};

namespace XR {

void InitVRMap() {
    // Sensible Quest Touch Plus defaults (can be overridden by controls.cfg).
    VRMap.fkForward       = VR_BTN_LS_UP;
    VRMap.fkBackward      = VR_BTN_LS_DOWN;
    VRMap.fkSLeft         = VR_BTN_LS_LEFT;
    VRMap.fkSRight        = VR_BTN_LS_RIGHT;
    VRMap.fkLeft          = VR_BTN_RS_LEFT;
    VRMap.fkRight         = VR_BTN_RS_RIGHT;
    VRMap.fkUp            = -1;
    VRMap.fkDown          = -1;
    VRMap.fkFire          = VR_BTN_TRIGGER_R;
    VRMap.fkShow          = VR_BTN_GRIP_R;
    VRMap.fkStrafe        = VR_BTN_THUMBSTICK_L;
    VRMap.fkJump          = VR_BTN_B;
    VRMap.fkRun           = VR_BTN_THUMBSTICK_R;
    VRMap.fkCrouch        = VR_BTN_A;
    VRMap.fkCall          = VR_BTN_X;
    VRMap.fkCCall         = VR_BTN_Y;
    VRMap.fkBinoc         = VR_BTN_GRIP_L;
}

const char* VRBtnName(int btn) {
    switch (btn) {
    case VR_BTN_TRIGGER_R:    return "Trig R";
    case VR_BTN_TRIGGER_L:    return "Trig L";
    case VR_BTN_GRIP_R:       return "Grip R";
    case VR_BTN_GRIP_L:       return "Grip L";
    case VR_BTN_THUMBSTICK_R: return "LS Clk R";
    case VR_BTN_THUMBSTICK_L: return "LS Clk L";
    case VR_BTN_A:            return "A";
    case VR_BTN_B:            return "B";
    case VR_BTN_X:            return "X";
    case VR_BTN_Y:            return "Y";
    case VR_BTN_LS_UP:        return "LS Up";
    case VR_BTN_LS_DOWN:      return "LS Dn";
    case VR_BTN_LS_LEFT:      return "LS L";
    case VR_BTN_LS_RIGHT:     return "LS R";
    case VR_BTN_RS_UP:        return "RS Up";
    case VR_BTN_RS_DOWN:      return "RS Dn";
    case VR_BTN_RS_LEFT:      return "RS L";
    case VR_BTN_RS_RIGHT:     return "RS R";
    default:                  return "(none)";
    }
}

void SampleVR() {
    if (!g_actionsReady || g_session == XR_NULL_HANDLE) return;

    struct Slot { int* vr; int vk; };
    Slot slots[] = {
        { &VRMap.fkForward,  KeyMap.fkForward  },
        { &VRMap.fkBackward, KeyMap.fkBackward },
        { &VRMap.fkUp,       KeyMap.fkUp       },
        { &VRMap.fkDown,     KeyMap.fkDown     },
        { &VRMap.fkLeft,     KeyMap.fkLeft     },
        { &VRMap.fkRight,    KeyMap.fkRight    },
        { &VRMap.fkFire,     KeyMap.fkFire     },
        { &VRMap.fkShow,     KeyMap.fkShow     },
        { &VRMap.fkSLeft,    KeyMap.fkSLeft    },
        { &VRMap.fkSRight,   KeyMap.fkSRight   },
        { &VRMap.fkStrafe,   KeyMap.fkStrafe   },
        { &VRMap.fkJump,     KeyMap.fkJump     },
        { &VRMap.fkRun,      KeyMap.fkRun      },
        { &VRMap.fkCrouch,   KeyMap.fkCrouch   },
        { &VRMap.fkCall,     KeyMap.fkCall     },
        { &VRMap.fkCCall,    KeyMap.fkCCall    },
        { &VRMap.fkBinoc,    KeyMap.fkBinoc    },
    };

    for (auto& s : slots) {
        int btn = *s.vr;
        if (btn < VR_BTN_FIRST || btn > VR_BTN_LAST) continue;
        bool held    = g_vrBtnHeld[btn];
        bool wasHeld = g_vrBtnPrev[btn];
        int  vk      = s.vk;
        if (vk <= 0 || vk >= 256) continue;
        // Edge-triggered actions (same pattern as Gamepad::DispatchAction)
        if (held && !wasHeld) {
            if (vk == KeyMap.fkBinoc) { ToggleBinocular(); continue; }
            if (vk == KeyMap.fkCCall) { ChangeCall();      continue; }
        }
        // Held actions: poke KeyboardState[]
        if (held) KeyboardState[vk] |=  128;
        else      KeyboardState[vk] &= ~128;
    }

    memcpy(g_vrBtnPrev, g_vrBtnHeld, sizeof(g_vrBtnHeld));

    // ── Right stick → analog mouselook (always-on, not VRMap-bound) ──────────
    // Left stick movement is handled above via VRMap virtual button codes
    // (VR_BTN_LS_UP/DOWN/LEFT/RIGHT) — threshold-detected in PollControllers().
    constexpr float kDeadzone  = 0.15f;
    constexpr float kLookSensX = 0.9f;
    constexpr float kLookSensY = 0.7f;
    float padScale = (OptMsSens > 0) ? (float)OptMsSens / 10.0f : 1.0f;

    auto Norm = [&](float v) -> float {
        float mag = v < 0 ? -v : v;
        if (mag < kDeadzone) return 0.f;
        float sign = v < 0 ? -1.f : 1.f;
        float t = (mag - kDeadzone) / (1.f - kDeadzone);
        if (t > 1.f) t = 1.f;
        return sign * t * t;
    };

    float rx = Norm(g_rightStick.x);
    float ry = Norm(g_rightStick.y);  // +Y = up; negate so up = look up
    g_sdlMouseDX += (int)( rx * kLookSensX * (float)TimeDt * padScale);
    g_sdlMouseDY += (int)(-ry * kLookSensY * (float)TimeDt * padScale);
}

int PollVRBtnEdge() {
    // Separate state table so the rebind UI doesn't corrupt SampleVR's prev state.
    static bool s_rebind[VR_BTN_LAST + 1] = {};
    for (int b = VR_BTN_FIRST; b <= VR_BTN_LAST; ++b) {
        bool active = g_vrBtnHeld[b];
        bool was    = s_rebind[b];
        s_rebind[b] = active;
        if (active && !was) return b;
    }
    return 0;
}

} // namespace XR
