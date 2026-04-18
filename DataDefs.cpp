// SOURCEPORT: data-driven dino/weapon overlay loader.
// See DataDefs.h for rationale and integration notes.

#include "DataDefs.h"
#include "Hunt.h"
#include "HotReload.h"
#include "VFS.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <memory>

// Forward-declared in Resources.cpp — re-runs _res.txt parse to restore
// retail baseline before re-layering JSON on hot reload.
extern void LoadResourcesScript();

namespace {

// ─── Minimal JSON parser ─────────────────────────────────────────────────────
// Supports: object, array, string ("..."), number, true/false/null, //
// line comments and /* block comments */. Silently returns null JValue on any
// parse error — we log a message to render.log and skip the file rather than
// crashing the game, because this is a modder-authored overlay.

struct JValue;
using JObject = std::vector<std::pair<std::string, std::shared_ptr<JValue>>>;
using JArray  = std::vector<std::shared_ptr<JValue>>;

enum class JKind { Null, Bool, Num, Str, Arr, Obj };

struct JValue {
    JKind kind = JKind::Null;
    bool        b = false;
    double      n = 0.0;
    std::string s;
    JArray      a;
    JObject     o;
};

struct JParser {
    const char* p;
    const char* end;
    bool ok = true;

    void skip() {
        for (;;) {
            while (p < end && (unsigned char)*p <= ' ') ++p;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (p < end && *p != '\n') ++p;
                continue;
            }
            if (p + 1 < end && p[0] == '/' && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
                if (p + 1 < end) p += 2;
                continue;
            }
            break;
        }
    }

    bool accept(char c) { skip(); if (p < end && *p == c) { ++p; return true; } return false; }
    void expect(char c) { if (!accept(c)) ok = false; }

    std::shared_ptr<JValue> parseVal() {
        skip();
        auto v = std::make_shared<JValue>();
        if (p >= end) { ok = false; return v; }
        char c = *p;
        if (c == '"' || c == '\'')          { v->kind = JKind::Str; v->s = parseStr(); }
        else if (c == '{')                   { v->kind = JKind::Obj; v->o = parseObj(); }
        else if (c == '[')                   { v->kind = JKind::Arr; v->a = parseArr(); }
        else if (c == '-' || (c >= '0' && c <= '9')) { v->kind = JKind::Num; v->n = parseNum(); }
        else if (!strncmp(p, "true", 4))     { v->kind = JKind::Bool; v->b = true;  p += 4; }
        else if (!strncmp(p, "false", 5))    { v->kind = JKind::Bool; v->b = false; p += 5; }
        else if (!strncmp(p, "null", 4))     { v->kind = JKind::Null; p += 4; }
        else ok = false;
        return v;
    }

    std::string parseStr() {
        std::string out;
        char q = *p++;
        while (p < end && *p != q) {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    case '"':  out.push_back('"');  break;
                    case '\'': out.push_back('\''); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    default:   out.push_back(*p);   break;
                }
                ++p;
            } else {
                out.push_back(*p++);
            }
        }
        if (p < end) ++p;
        return out;
    }

    double parseNum() {
        char* ep = nullptr;
        double v = strtod(p, &ep);
        if (ep == p) { ok = false; return 0.0; }
        p = ep;
        return v;
    }

    JObject parseObj() {
        JObject out;
        expect('{');
        skip();
        if (accept('}')) return out;
        while (ok) {
            skip();
            if (p >= end || (*p != '"' && *p != '\'')) { ok = false; break; }
            std::string k = parseStr();
            expect(':');
            auto v = parseVal();
            out.emplace_back(std::move(k), std::move(v));
            skip();
            if (accept('}')) break;
            if (!accept(',')) { ok = false; break; }
        }
        return out;
    }

    JArray parseArr() {
        JArray out;
        expect('[');
        skip();
        if (accept(']')) return out;
        while (ok) {
            out.push_back(parseVal());
            skip();
            if (accept(']')) break;
            if (!accept(',')) { ok = false; break; }
        }
        return out;
    }
};

std::shared_ptr<JValue> ParseJsonFile(const char* path) {
    FILE* f = VFS::fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string buf;
    buf.resize((size_t)sz);
    if (sz > 0) fread(&buf[0], 1, (size_t)sz, f);
    fclose(f);

    JParser jp{ buf.data(), buf.data() + buf.size() };
    auto v = jp.parseVal();
    if (!jp.ok || !v) {
        char msg[256];
        sprintf(msg, "[DataDefs] Parse error in %s\n", path);
        PrintLog(msg);
        return nullptr;
    }
    return v;
}

// ─── Object-field helpers ────────────────────────────────────────────────────

const JValue* Lookup(const JObject& o, const char* key) {
    for (auto& kv : o) if (kv.first == key) return kv.second.get();
    return nullptr;
}

bool AsNum(const JValue* v, double& out) {
    if (!v) return false;
    if (v->kind == JKind::Num)  { out = v->n; return true; }
    if (v->kind == JKind::Bool) { out = v->b ? 1.0 : 0.0; return true; }
    return false;
}
bool AsStr(const JValue* v, std::string& out) {
    if (v && v->kind == JKind::Str) { out = v->s; return true; }
    return false;
}
bool AsBool(const JValue* v, bool& out) {
    if (!v) return false;
    if (v->kind == JKind::Bool) { out = v->b; return true; }
    if (v->kind == JKind::Num)  { out = v->n != 0.0; return true; }
    return false;
}

// ─── Applying overlays ───────────────────────────────────────────────────────

int FindDinoIndex(const JObject& entry) {
    double id;
    if (AsNum(Lookup(entry, "id"), id)) {
        int i = (int)id;
        if (i >= 0 && i < TotalC) return i;
    }
    double ai;
    if (AsNum(Lookup(entry, "ai"), ai)) {
        int want = (int)ai;
        for (int i = 0; i < TotalC; ++i)
            if (DinoInfo[i].AI == want) return i;
    }
    std::string name;
    if (AsStr(Lookup(entry, "name"), name)) {
        for (int i = 0; i < TotalC; ++i)
            if (strcmp(DinoInfo[i].Name, name.c_str()) == 0) return i;
    }
    return -1;
}

int FindWeaponIndex(const JObject& entry) {
    double id;
    if (AsNum(Lookup(entry, "id"), id)) {
        int i = (int)id;
        if (i >= 0 && i < TotalW) return i;
    }
    std::string name;
    if (AsStr(Lookup(entry, "name"), name)) {
        for (int i = 0; i < TotalW; ++i)
            if (strcmp(WeapInfo[i].Name, name.c_str()) == 0) return i;
    }
    return -1;
}

void ApplyDino(const JObject& e, int idx) {
    TDinoInfo& d = DinoInfo[idx];
    double n; bool b; std::string s;
    if (AsStr(Lookup(e, "name"),      s)) { strncpy(d.Name,  s.c_str(), sizeof(d.Name) -1); d.Name [sizeof(d.Name) -1] = 0; }
    if (AsStr(Lookup(e, "file"),      s)) { strncpy(d.FName, s.c_str(), sizeof(d.FName)-1); d.FName[sizeof(d.FName)-1] = 0; }
    if (AsStr(Lookup(e, "pic"),       s)) { strncpy(d.PName, s.c_str(), sizeof(d.PName)-1); d.PName[sizeof(d.PName)-1] = 0; }
    if (AsNum(Lookup(e, "ai"),        n)) d.AI        = (int)n;
    if (AsNum(Lookup(e, "health"),    n)) d.Health0   = (int)n;
    if (AsNum(Lookup(e, "basescore"), n)) d.BaseScore = (int)n;
    if (AsNum(Lookup(e, "mass"),      n)) d.Mass      = (float)n;
    if (AsNum(Lookup(e, "length"),    n)) d.Length    = (float)n;
    if (AsNum(Lookup(e, "radius"),    n)) d.Radius    = (float)n;
    if (AsNum(Lookup(e, "smell"),     n)) d.SmellK    = (float)n;
    if (AsNum(Lookup(e, "hear"),      n)) d.HearK     = (float)n;
    if (AsNum(Lookup(e, "look"),      n)) d.LookK     = (float)n;
    if (AsNum(Lookup(e, "scale0"),    n)) d.Scale0    = (int)n;
    if (AsNum(Lookup(e, "scaleA"),    n)) d.ScaleA    = (int)n;
    if (AsNum(Lookup(e, "shipdelta"), n)) d.ShDelta   = (float)n;
    if (AsBool(Lookup(e, "danger"),   b)) d.DangerCall = b ? TRUE : FALSE;

    // Keep the AI→index lookup in sync.
    if (d.AI >= 0 && d.AI < 64) AI_to_CIndex[d.AI] = idx;
}

void ApplyWeapon(const JObject& e, int idx) {
    TWeapInfo& w = WeapInfo[idx];
    double n; bool b; std::string s;
    if (AsStr(Lookup(e, "name"),   s)) { strncpy(w.Name,   s.c_str(), sizeof(w.Name)  -1); w.Name  [sizeof(w.Name)  -1] = 0; }
    if (AsStr(Lookup(e, "file"),   s)) { strncpy(w.FName,  s.c_str(), sizeof(w.FName) -1); w.FName [sizeof(w.FName) -1] = 0; }
    if (AsStr(Lookup(e, "pic"),    s)) { strncpy(w.BFName, s.c_str(), sizeof(w.BFName)-1); w.BFName[sizeof(w.BFName)-1] = 0; }
    if (AsNum(Lookup(e, "power"),  n)) w.Power  = (float)n;
    if (AsNum(Lookup(e, "prec"),   n)) w.Prec   = (float)n;
    if (AsNum(Lookup(e, "loud"),   n)) w.Loud   = (float)n;
    if (AsNum(Lookup(e, "rate"),   n)) w.Rate   = (float)n;
    if (AsNum(Lookup(e, "shots"),  n)) w.Shots  = (int)n;
    if (AsNum(Lookup(e, "reload"), n)) w.Reload = (int)n;
    if (AsNum(Lookup(e, "trace"),  n)) w.TraceC = (int)n - 1;
    if (AsBool(Lookup(e, "optic"), b)) w.Optic  = b ? 1 : 0;
    if (AsBool(Lookup(e, "fall"),  b)) w.Fall   = b ? 1 : 0;
}

void LoadDinoOverlay() {
    auto root = ParseJsonFile("HUNTDAT\\dinos.json");
    if (!root) return;
    if (root->kind != JKind::Obj) return;
    auto* arrV = Lookup(root->o, "dinosaurs");
    if (!arrV || arrV->kind != JKind::Arr) return;

    int applied = 0, added = 0;
    for (auto& el : arrV->a) {
        if (!el || el->kind != JKind::Obj) continue;
        int idx = FindDinoIndex(el->o);
        if (idx < 0) {
            if (TotalC >= (int)(sizeof(DinoInfo) / sizeof(DinoInfo[0]))) {
                PrintLog("[DataDefs] DinoInfo full; skipping extra entry\n");
                continue;
            }
            idx = TotalC++;
            memset(&DinoInfo[idx], 0, sizeof(DinoInfo[idx]));
            ++added;
        } else {
            ++applied;
        }
        ApplyDino(el->o, idx);
    }
    char msg[192];
    sprintf(msg, "[DataDefs] dinos.json: %d overrides, %d added\n", applied, added);
    PrintLog(msg);
}

void LoadWeaponOverlay() {
    auto root = ParseJsonFile("HUNTDAT\\weapons.json");
    if (!root) return;
    if (root->kind != JKind::Obj) return;
    auto* arrV = Lookup(root->o, "weapons");
    if (!arrV || arrV->kind != JKind::Arr) return;

    int applied = 0, added = 0;
    for (auto& el : arrV->a) {
        if (!el || el->kind != JKind::Obj) continue;
        int idx = FindWeaponIndex(el->o);
        if (idx < 0) {
            if (TotalW >= (int)(sizeof(WeapInfo) / sizeof(WeapInfo[0]))) {
                PrintLog("[DataDefs] WeapInfo full; skipping extra entry\n");
                continue;
            }
            idx = TotalW++;
            memset(&WeapInfo[idx], 0, sizeof(WeapInfo[idx]));
            ++added;
        } else {
            ++applied;
        }
        ApplyWeapon(el->o, idx);
    }
    char msg[192];
    sprintf(msg, "[DataDefs] weapons.json: %d overrides, %d added\n", applied, added);
    PrintLog(msg);
}

} // namespace

namespace DataDefs {

void ApplyJsonOverlays() {
    LoadDinoOverlay();
    LoadWeaponOverlay();
}

void RegisterHotReload() {
    static bool s_registered = false;
    if (s_registered) return;
    s_registered = true;

    HotReload::Watch("HUNTDAT\\dinos.json", []() {
        // Re-parse _res.txt baseline then re-layer JSON overlay.
        LoadResourcesScript();
        ApplyJsonOverlays();
        PrintLog("[HotReload] dinos.json reloaded\n");
    });
    HotReload::Watch("HUNTDAT\\weapons.json", []() {
        LoadResourcesScript();
        ApplyJsonOverlays();
        PrintLog("[HotReload] weapons.json reloaded\n");
    });
}

} // namespace DataDefs
