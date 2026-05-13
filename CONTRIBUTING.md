# Contributing to OpenCarnivores

This guide documents the development workflow, code quality standards, and contribution process for OpenCarnivores.

## Development Setup

1. **Clone the repository**
2. **Install dependencies:**
   - CMake 3.20+
   - Visual Studio 2022 (MSVC) or gcc/clang
   - SDL2 development headers
   - OpenGL libraries
   - clang-tidy for static analysis

3. **Build the project:**
   ```bash
   cd build
   cmake .. -DRENDERER=opengl
   ```

## Coding Standards

See [CLAUDE.md](CLAUDE.md) for project-specific rules:
- C++17 standard required
- Prefer STL containers over raw arrays
- Use `#pragma once` for headers
- Comment behavioral changes with `// SOURCEPORT: <description>`
- Maintain mod compatibility (never remove retail asset format parsers)

## Pre-Commit Workflow

### 1. Code Changes

Make your changes following the coding standards above.

### 2. Build & Verify

```bash
cd build
cmake .. -DRENDERER=opengl
cmake --build . --config Debug
```

Ensure the build produces 0 errors.

### 3. **REQUIRED: Run Static Analysis (clang-tidy)**

Before committing, you **must** run clang-tidy on your modified files and fix all critical findings:

```bash
# Run on files you modified
clang-tidy -p build/ --checks='bugprone-*,clang-analyzer-*,-clang-analyzer-security.insecureAPI.*' YOUR_MODIFIED_FILE.cpp
```

### 4. Critical Findings (MUST FIX before commit)

Do not commit with unresolved critical issues. These indicate real bugs:

**Virtual calls in destructors**
```cpp
// ❌ BAD - undefined behavior during destruction
~MyClass() { 
    Shutdown();  // virtual method, vtable is undefined
}

// ✅ GOOD - explicit non-virtual dispatch
~MyClass() { 
    MyClass::Shutdown();  // calls derived class method directly
}
```

**Integer overflow in size calculations**
```cpp
// ❌ BAD - int * int can overflow before conversion
std::vector<uint32_t> pixels(width * height);

// ✅ GOOD - cast to size_t before multiplication
std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
```

**Operator precedence with logical NOT**
```cpp
// ❌ BAD - NOT applies only to left operand
if (!flag & 1) { ... }

// ✅ GOOD - parentheses clarify intent
if (!(flag & 1)) { ... }
```

**Goto jumping over variable initialization**
```cpp
// ❌ BAD - C++ doesn't allow jumping over initialized variables
if (error) goto cleanup;
Vector3d pos = GetPosition();
cleanup:
    CleanUp();

// ✅ GOOD - declare before any possible goto target
Vector3d pos;
if (error) goto cleanup;
pos = GetPosition();
cleanup:
    CleanUp();
```

**Unused variables**
```cpp
// ❌ BAD - assigned but never used
float unused = CalculateValue();

// ✅ GOOD - either use it or remove it
// If intentionally unused, add a comment explaining why
```

### 5. Non-Critical Findings (Warnings OK)

These warnings are acceptable and don't block commits:

- **Reserved identifiers** — legacy `_StructName` style from 1999 codebase (too many to refactor)
- **Narrowing conversions** — int→float type coercions in legacy code
- **Microsoft extensions** — non-standard goto behavior (from original code)

Document these with comments if they're noisy, but don't let them block progress.

### 6. Commit Message

Use a clear, descriptive commit message:

```
Subsystem: Brief description of change

Longer explanation if needed. Reference issue numbers or
architectural decisions. Keep lines to 72 characters.

- Bullet points for multiple changes
- One change per line
```

Example:
```
Rendering: Fix virtual call in RendererGL destructor

Explicitly call RendererGL::Shutdown() instead of relying on virtual
dispatch during object destruction, which results in undefined behavior
in C++. This resolves clang-tidy bugprone-optin.cplusplus.VirtualCall
warning.

- Fixes integer overflow in texture size calculations
- Adds explicit casts for size_t multiplication
```

### 7. Push to GitHub

```bash
git push origin branch-name
```

## Running Tests

After building, you can test the executable:

```bash
./build/Debug/OpenCarnivores.exe
```

(VR hardware required for full VR testing; flatscreen mode works without.)

## Documentation

Keep documentation in sync with code changes:

- Update [ARCHITECTURE.md](ARCHITECTURE.md) for structural changes
- Update domain guides ([RENDERING.md](RENDERING.md), [AUDIO.md](AUDIO.md), etc.) for feature changes
- Update [ROADMAP.md](ROADMAP.md) when completing/changing planned work
- Add `// SOURCEPORT:` comments in code for non-obvious changes

## Questions?

Refer to:
- [CLAUDE.md](CLAUDE.md) — Project constraints and coding rules
- [RENDERING.md](RENDERING.md) — Rendering architecture details
- [AUDIO.md](AUDIO.md) — Audio system documentation
- [ROADMAP.md](ROADMAP.md) — Planned features and known issues
