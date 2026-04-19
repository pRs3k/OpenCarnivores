#pragma once
// SOURCEPORT: persist the KeyMap (action→VK) to disk so rebinds made in the
// Options→Controls panel survive a restart. The hot path (ProcessControls
// reading KeyboardState[KeyMap.fk*]) is unchanged — this module only
// serialises the struct.
//
// Config format is a line-oriented text file, one `action=VK` pair per line,
// `#`-prefixed comments allowed. VK is stored as a decimal Win32 virtual-key
// code so the file is stable across keyboard layouts.
//   Forward=87
//   Fire=1
//   Jump=32
// Unknown actions and malformed lines are skipped with a log entry; missing
// actions keep whatever defaults InitKeyMap set.

namespace Bindings {

// Call once on startup, AFTER InitKeyMap has populated KeyMap with defaults,
// so partial config files only overwrite the keys they name.
void Load();

// Write the current KeyMap to controls.cfg. Called when leaving the Options
// screen (the UI already mutates KeyMap in place via its Binding table).
void Save();

// Restore every action in KeyMap and PadMap to the hardcoded defaults. Used
// by the Options > Controls "Reset" button and by the first-run init path.
void ResetToDefaults();

} // namespace Bindings
