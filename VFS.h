// SOURCEPORT: virtual filesystem for mod mounting.
//
// Reads the enabled-mods list from mods.cfg, enumerates `mods/<name>/`
// subtrees as higher-priority overlays on top of the retail game folder,
// and exposes a path resolver used by every asset-loading call site.
//
// Mount order (highest priority first):
//   1. mods/<first-line-of-mods.cfg>/<path>
//   2. mods/<second-line>/<path>
//   ...
//   N. <path>   (retail — always the fallback)
//
// Retail files are never touched. Mods that ship a modified MENUM.TGA /
// AREA1.RSC / TREX.CAR / etc. just need to mirror the original layout
// under their pack folder — see README "mods/ folder" section.

#pragma once

#include <string>
#include <cstdio>

namespace VFS {

// Read mods.cfg and build the mount list. Safe to call more than once —
// re-scans from disk so toggles in the MODS menu take effect on next Init.
void Init();

// Resolve a read path. If any mounted mod has a file at `<mod>/<path>`,
// return that. Otherwise return `path` unchanged. Case-insensitive on the
// backslash/forward-slash axis — we normalize internally.
std::string ResolveRead(const char* path);

// Convenience wrapper — resolves the path, then calls ::fopen.
std::FILE* fopen(const char* path, const char* mode);

// For dev builds: returns the number of mods currently mounted.
int MountCount();

} // namespace VFS
