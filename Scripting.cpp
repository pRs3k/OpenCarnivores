// SOURCEPORT: Lua 5.4 scripting layer. See Scripting.h for design.
//
// This file is the ONLY translation unit that touches the Lua C API; the
// rest of the codebase sees only the opaque `Scripting::` namespace. Keeps
// the #include <lua.h> blast radius small and makes it easy to strip the
// module by removing one file + one CMake entry.

#include "Scripting.h"
#include "Hunt.h"

#include <lua.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

lua_State* L        = nullptr;
bool       g_active = false;

// ── tiny helpers ────────────────────────────────────────────────────────────

void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

void Logf(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log(buf);
}

// Push a TCharacter as a Lua table. Fields are a read-only snapshot —
// mutating the table has no effect on the engine. Modders who need to
// change state use the `oc.*` helpers registered below.
void PushCharacter(lua_State* S, const TCharacter* c, int charIndex) {
    lua_createtable(S, 0, 10);
    lua_pushinteger(S, charIndex);          lua_setfield(S, -2, "id");
    lua_pushinteger(S, c->CType);           lua_setfield(S, -2, "ctype");
    lua_pushinteger(S, c->AI);              lua_setfield(S, -2, "ai");
    lua_pushinteger(S, c->Health);          lua_setfield(S, -2, "health");
    lua_pushinteger(S, c->State);           lua_setfield(S, -2, "state");
    lua_pushnumber (S, c->pos.x);           lua_setfield(S, -2, "x");
    lua_pushnumber (S, c->pos.y);           lua_setfield(S, -2, "y");
    lua_pushnumber (S, c->pos.z);           lua_setfield(S, -2, "z");
    lua_pushnumber (S, c->alpha);           lua_setfield(S, -2, "alpha");
    lua_pushnumber (S, c->beta);            lua_setfield(S, -2, "beta");
    // Dino type name (from _res.txt) when the CType index points at a real
    // DinoInfo entry — gives scripts a stable string key for "trex", "velo",
    // etc. without having to hard-code numeric ids that shift between mods.
    if (c->CType >= 0 && c->CType < 32 && DinoInfo[c->CType].Name[0]) {
        lua_pushstring(S, DinoInfo[c->CType].Name);
        lua_setfield(S, -2, "name");
    }
}

// Pop the error string off the Lua stack and log it. Use after a pcall failure.
void LogPCallError(const char* where) {
    const char* err = lua_tostring(L, -1);
    Logf("[Lua] error in %s: %s\n", where, err ? err : "(unknown)");
    lua_pop(L, 1);
}

// Invoke a global Lua function named `fn` if it exists. Caller pushes any
// args AFTER this call returns 0 — we push the function onto the stack and
// return the number of args it expects (always 0 here; caller pushes via
// their own lua_push*). If the function doesn't exist, returns -1 and the
// stack is untouched so the caller can skip.
int BeginCall(const char* fn) {
    if (!L) return -1;
    lua_getglobal(L, fn);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return -1; }
    return 0;
}

// Finish a call started by BeginCall: pcall with `nargs` and log any error.
void EndCall(const char* fn, int nargs) {
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) LogPCallError(fn);
}

// ── oc.* bindings ───────────────────────────────────────────────────────────

int l_log(lua_State* S) {
    const char* s = luaL_optstring(S, 1, "");
    Logf("[Lua] %s\n", s);
    return 0;
}

int l_message(lua_State* S) {
    const char* s = luaL_optstring(S, 1, "");
    AddMessage(const_cast<LPSTR>(s));
    return 0;
}

// oc.setHealth(id, hp) — write into Characters[id].Health. Bounds-checked
// against ChCount so a script typo can't scribble into random memory.
int l_setHealth(lua_State* S) {
    int id = (int)luaL_checkinteger(S, 1);
    int hp = (int)luaL_checkinteger(S, 2);
    if (id >= 0 && id < ChCount) {
        if (hp < 0) hp = 0;
        Characters[id].Health = hp;
    }
    return 0;
}

// oc.getHealth(id) — returns current HP, or -1 for invalid id.
int l_getHealth(lua_State* S) {
    int id = (int)luaL_checkinteger(S, 1);
    lua_pushinteger(S, (id >= 0 && id < ChCount) ? Characters[id].Health : -1);
    return 1;
}

// oc.playerPos() — returns x, y, z of the player.
int l_playerPos(lua_State* S) {
    lua_pushnumber(S, PlayerX);
    lua_pushnumber(S, PlayerY);
    lua_pushnumber(S, PlayerZ);
    return 3;
}

// oc.dinoCount() / oc.characterCount() — how many active entries in the
// Characters array. Scripts walk [0, count) themselves.
int l_dinoCount(lua_State* S) {
    lua_pushinteger(S, ChCount);
    return 1;
}

// oc.getCharacter(id) — pushes the same snapshot table the event hooks
// receive, for scripts that want to poll instead of react.
int l_getCharacter(lua_State* S) {
    int id = (int)luaL_checkinteger(S, 1);
    if (id < 0 || id >= ChCount) { lua_pushnil(S); return 1; }
    PushCharacter(S, &Characters[id], id);
    return 1;
}

void RegisterAPI() {
    lua_newtable(L);
    #define BIND(name, fn) lua_pushcfunction(L, fn); lua_setfield(L, -2, name)
    BIND("log",            l_log);
    BIND("message",        l_message);
    BIND("setHealth",      l_setHealth);
    BIND("getHealth",      l_getHealth);
    BIND("playerPos",      l_playerPos);
    BIND("dinoCount",      l_dinoCount);
    BIND("getCharacter",   l_getCharacter);
    #undef BIND
    lua_setglobal(L, "oc");
}

// ── script directory walk ──────────────────────────────────────────────────

// Load every `scripts/*.lua` file. Errors in one file don't abort the others
// so a typo in one mod script doesn't silently kill every other mod.
void LoadAllScripts() {
    #ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("scripts\\*.lua", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("[Lua] no scripts/ directory — scripting idle\n");
        return;
    }
    int loaded = 0, failed = 0;
    do {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "scripts\\%s", fd.cFileName);
        if (luaL_dofile(L, path) != LUA_OK) {
            LogPCallError(path);
            ++failed;
        } else {
            Logf("[Lua] loaded %s\n", path);
            ++loaded;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (loaded > 0) g_active = true;
    Logf("[Lua] %d script(s) loaded, %d failed\n", loaded, failed);
    #endif
}

} // anon

namespace Scripting {

void Init() {
    if (L) return;
    L = luaL_newstate();
    if (!L) { Log("[Lua] luaL_newstate failed\n"); return; }
    luaL_openlibs(L);
    RegisterAPI();
    LoadAllScripts();

    // Optional one-shot OnInit() so scripts can do startup work without
    // piggy-backing on the first spawn event.
    if (BeginCall("OnInit") == 0) EndCall("OnInit", 0);
}

void Shutdown() {
    if (!L) return;
    lua_close(L);
    L = nullptr;
    g_active = false;
}

bool Active() { return g_active; }

void OnSpawn(const TCharacter* ch, int charIndex) {
    if (!ch || BeginCall("OnSpawn") != 0) return;
    PushCharacter(L, ch, charIndex);
    EndCall("OnSpawn", 1);
}

void OnDamage(const TCharacter* ch, int charIndex, int amount) {
    if (!ch || BeginCall("OnDamage") != 0) return;
    PushCharacter(L, ch, charIndex);
    lua_pushinteger(L, amount);
    EndCall("OnDamage", 2);
}

void OnFire(int weaponIndex) {
    if (BeginCall("OnFire") != 0) return;
    lua_pushinteger(L, weaponIndex);
    EndCall("OnFire", 1);
}

} // namespace Scripting
