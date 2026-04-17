// SOURCEPORT: static model-override loader (.obj today, .gltf placeholder).
// See ModelOverrides.h for scope and rationale.

#include "ModelOverrides.h"
#include "hunt.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Log(const char* msg) { PrintLog(const_cast<char*>(msg)); }

bool EndsWithICase(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

// Parse the v/t/n corner tokens used by `f` lines. Accepts "v", "v/t",
// "v//n", "v/t/n". 1-based indices; negative (relative) indices not
// handled — rare in authored OBJs and easy to avoid on the export side.
struct Corner { int v; int t; };
Corner ParseCorner(const char* tok) {
    Corner c{0, 0};
    const char* p = tok;
    c.v = std::atoi(p);
    const char* slash = std::strchr(p, '/');
    if (slash && slash[1] != '/' && slash[1] != '\0')
        c.t = std::atoi(slash + 1);
    return c;
}

// Minimal OBJ parser — positions, texcoords, and triangulated faces (fans).
bool LoadOBJ(TModel* mptr, const char* objPath)
{
    FILE* f = std::fopen(objPath, "rb");
    if (!f) return false;

    std::vector<float> vx, vy, vz;  // positions (1-based in file; we push index 0 stub)
    std::vector<float> uu, vv;      // texcoords

    vx.push_back(0); vy.push_back(0); vz.push_back(0);
    uu.push_back(0); vv.push_back(0);

    struct Tri { int vi[3]; int ti[3]; };
    std::vector<Tri> tris;

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        // Skip leading whitespace
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        if (p[0] == 'v' && p[1] == ' ') {
            float x, y, z;
            if (std::sscanf(p + 2, "%f %f %f", &x, &y, &z) == 3) {
                vx.push_back(x); vy.push_back(y); vz.push_back(z);
            }
        } else if (p[0] == 'v' && p[1] == 't') {
            float u, v;
            if (std::sscanf(p + 2, "%f %f", &u, &v) == 2) {
                uu.push_back(u); vv.push_back(v);
            }
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            // Tokenise the face's corners; triangulate as a fan (c0, c[i], c[i+1]).
            std::vector<Corner> corners;
            char* ctx = nullptr;
            char* tok = std::strtok(p + 2, " \t\r\n");
            while (tok) {
                corners.push_back(ParseCorner(tok));
                tok = std::strtok(nullptr, " \t\r\n");
            }
            for (size_t i = 1; i + 1 < corners.size(); ++i) {
                Tri tri;
                tri.vi[0] = corners[0].v;     tri.ti[0] = corners[0].t;
                tri.vi[1] = corners[i].v;     tri.ti[1] = corners[i].t;
                tri.vi[2] = corners[i + 1].v; tri.ti[2] = corners[i + 1].t;
                tris.push_back(tri);
            }
        }
        // v.n, o, g, s, mtllib, usemtl — ignored.
    }
    std::fclose(f);

    const int verts = (int)vx.size() - 1;
    const int faces = (int)tris.size();
    if (verts <= 0 || faces <= 0) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "Model override rejected (no geometry): %s\n", objPath);
        Log(msg);
        return false;
    }
    if (verts > 1024 || faces > 1024) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
            "Model override rejected (%d verts / %d faces > 1024 cap): %s\n",
            verts, faces, objPath);
        Log(msg);
        return false;
    }

    // Populate in-place. OBJ coordinate system: right-handed Y-up. The retail
    // TModel post-multiplies z by -1 during load; by the time we run here the
    // mesh is already in the engine's final coord system, so author OBJs in
    // Blender-default axes (Y up, -Z forward) and export as "OBJ (legacy)"
    // with -Z forward / Y up and they drop in with no manual flip.
    for (int i = 0; i < verts; ++i) {
        mptr->gVertex[i].x = vx[i + 1];
        mptr->gVertex[i].y = vy[i + 1];
        mptr->gVertex[i].z = -vz[i + 1];
        mptr->gVertex[i].owner = 0;
        mptr->gVertex[i].hide  = 0;
    }
    mptr->VCount = verts;

    // Faces: UV space in TModel (post-CorrectModel) is normalised 0..1, with
    // image-origin V (top-down). OBJ spec is bottom-up — flip V.
    for (int i = 0; i < faces; ++i) {
        TFacef& out = mptr->gFacef[i];
        const Tri& t = tris[i];
        out.v1 = t.vi[0] - 1;
        out.v2 = t.vi[1] - 1;
        out.v3 = t.vi[2] - 1;
        auto getU = [&](int ti){ return (ti > 0 && ti < (int)uu.size()) ? uu[ti]         : 0.f; };
        auto getV = [&](int ti){ return (ti > 0 && ti < (int)vv.size()) ? 1.f - vv[ti]   : 0.f; };
        out.tax = getU(t.ti[0]); out.tay = getV(t.ti[0]);
        out.tbx = getU(t.ti[1]); out.tby = getV(t.ti[1]);
        out.tcx = getU(t.ti[2]); out.tcy = getV(t.ti[2]);
        out.Flags    = sfOpacity | sfNeedVC;
        out.DMask    = 0xFFFF;
        out.Distant  = 0;
        out.Next     = 0;
        out.group    = 0;
        std::memset(out.reserv, 0, sizeof(out.reserv));
    }
    mptr->FCount = faces;

    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "Model override loaded: %s (%d verts, %d tris)\n",
        objPath, verts, faces);
    Log(msg);
    return true;
}

bool LoadGLTFStub(TModel* /*mptr*/, const char* path) {
    char msg[512];
    std::snprintf(msg, sizeof(msg),
        "glTF override skipped (loader not yet implemented): %s\n", path);
    Log(msg);
    return false;
}

} // anon

namespace ModelOverrides {

bool TrySiblingOBJ(TModel* mptr, const char* sourcePath)
{
    if (!mptr || !sourcePath) return false;
    std::string p = sourcePath;
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string obj = p.substr(0, dot) + ".obj";
    return LoadOBJ(mptr, obj.c_str());
}

bool TrySiblingAny(TModel* mptr, const char* sourcePath)
{
    if (!mptr || !sourcePath) return false;
    std::string p = sourcePath;
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string stem = p.substr(0, dot);

    if (LoadOBJ(mptr, (stem + ".obj").c_str())) return true;

    // Only warn for glTF if one actually exists — silent miss otherwise.
    for (const char* ext : { ".gltf", ".glb" }) {
        std::string candidate = stem + ext;
        FILE* f = std::fopen(candidate.c_str(), "rb");
        if (f) { std::fclose(f); return LoadGLTFStub(mptr, candidate.c_str()); }
    }
    return false;
}

} // namespace ModelOverrides
