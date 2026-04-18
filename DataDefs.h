// SOURCEPORT: data-driven dino/weapon definitions.
//
// Reads JSON overlays from HUNTDAT\dinos.json and HUNTDAT\weapons.json (if
// present) and merges them into DinoInfo[]/WeapInfo[] AFTER _res.txt has been
// parsed. Each entry can reference an existing retail definition (by "id" or
// "name") to override specific fields, or add a new entry by leaving both
// matchers unset.
//
// Applied as an additive layer so the retail _res.txt remains authoritative
// when no JSON is present — zero behavior change for vanilla installs.
//
// Hot-reload is wired through HotReload::Watch: edit and save, the running
// game re-applies the JSON overlay (re-running _res.txt first to restore the
// baseline before re-layering).

#pragma once

namespace DataDefs {

// Load HUNTDAT\dinos.json and HUNTDAT\weapons.json into the DinoInfo[]/
// WeapInfo[] arrays. Safe to call more than once (idempotent re-apply).
void ApplyJsonOverlays();

// Register HotReload watches for the JSON files. The callback re-runs the
// retail _res.txt parse and then re-layers the JSON overlay.
void RegisterHotReload();

} // namespace DataDefs
