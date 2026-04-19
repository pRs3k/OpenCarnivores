#pragma once
// SOURCEPORT: Lua 5.4 scripting layer.
//
// Opens modding to non-C++ devs via three event hooks fired from the engine:
//   OnSpawn(ch)          — whenever ResetCharacter() runs (dinos, the player)
//   OnDamage(ch, amount) — whenever the player's bullet reduces a dino's HP
//   OnFire(weapon)       — whenever the player successfully discharges a weapon
//
// All `scripts/*.lua` files are loaded once at startup; globally-named
// functions `OnSpawn`/`OnDamage`/`OnFire` are invoked on each event. The
// Character argument is a plain Lua table snapshot (read-only) with the
// fields we expose from TCharacter — modders who want to mutate game state
// call helper functions in the `oc` global table.
//
// This layer is entirely optional: if `scripts/` is absent or Lua init
// fails, the engine runs exactly as before with zero overhead.

struct _TCharacter;
typedef struct _TCharacter TCharacter;

namespace Scripting {

// Open the Lua VM, register the `oc` helper table, and load every .lua file
// under `scripts/`. Safe to call multiple times — later calls no-op.
void Init();

// Close the VM. No-op if Init never succeeded.
void Shutdown();

// Event dispatchers. Each is a cheap guard-then-noop when no Lua function
// with the matching name exists, so call sites can fire them unconditionally.
void OnSpawn(const TCharacter* ch, int charIndex);
void OnDamage(const TCharacter* ch, int charIndex, int amount);
void OnFire(int weaponIndex);

// True if at least one .lua file loaded successfully. Used by the log line
// in Hunt2.cpp so the user can confirm their scripts took effect.
bool Active();

} // namespace Scripting
