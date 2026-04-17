#pragma once
// SOURCEPORT: static model-geometry override registry.
//
// When a .3DF asset is loaded, we probe for a sibling file in a modern,
// tool-friendly format and — if present — replace the TModel's geometry
// (gVertex / gFace / VCount / FCount) in place while keeping the retail
// texture, HUD placement, and render path intact. Pair with a PNG/DDS
// TextureOverride for matching art.
//
// Scope for this phase: static meshes only (.3DF — sun, moon, compass,
// binoculars, map props). Animated .CAR characters are deliberately NOT
// overridden here because the retail vertex-morph animation stream is
// indexed into the original vertex array; swapping the mesh would
// desynchronise every keyframe.
//
// Supported formats:
//   * .obj  (Wavefront)   — positions + UVs + triangulated faces. Fully
//                            implemented. UVs assumed 0..1 (v flipped to
//                            match image origin).
//   * .gltf / .glb        — parsed on a best-effort basis; current build
//                            logs "unsupported" so the registry is ready
//                            for a future cgltf-backed loader without
//                            having to re-plumb the call sites.

struct TagMODEL;
typedef struct TagMODEL TModel;

namespace ModelOverrides {

// Try to load "<stem>.obj" next to sourcePath and replace mptr's geometry.
// Returns true on success.
bool TrySiblingOBJ(TModel* mptr, const char* sourcePath);

// Try .obj, then .gltf, then .glb. Returns true on the first hit.
bool TrySiblingAny(TModel* mptr, const char* sourcePath);

} // namespace ModelOverrides
