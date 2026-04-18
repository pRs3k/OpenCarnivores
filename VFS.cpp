// SOURCEPORT: VFS implementation. See VFS.h for rationale.

#include "VFS.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

// Global PrintLog lives in Resources.cpp — forward-declare at global scope so
// the namespaced VFS functions below don't accidentally qualify it.
extern "C" { /* nothing */ }
extern void PrintLog(char*);

namespace {

std::vector<std::string> g_mounts;  // ordered list of "mods/<name>" prefixes

// Read mods.cfg and return the list of enabled pack names in listed order.
std::vector<std::string> LoadEnabledList() {
    std::vector<std::string> out;
    std::FILE* f = std::fopen("mods.cfg", "r");
    if (!f) return out;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        int n = (int)std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' ||
                         line[n-1] == ' '  || line[n-1] == '\t')) line[--n] = 0;
        // Skip comments and blanks.
        if (n == 0 || line[0] == '#') continue;
        out.emplace_back(line);
    }
    std::fclose(f);
    return out;
}

// Build "<prefix>/<path>" with forward slashes, avoiding double separators.
std::string Join(const std::string& prefix, const char* path) {
    std::string out = prefix;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
    // Strip any leading slashes from `path` — paths like "HUNTDAT\\X" are relative.
    const char* p = path;
    while (*p == '/' || *p == '\\') ++p;
    out.append(p);
    return out;
}

} // namespace

namespace VFS {

void Init() {
    g_mounts.clear();
    namespace fs = std::filesystem;
    std::error_code ec;

    std::vector<std::string> enabled = LoadEnabledList();
    for (auto& name : enabled) {
        fs::path p = fs::path("mods") / name;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            // Store with forward slashes — std::filesystem/CreateFile both accept either.
            std::string s = p.generic_string();
            g_mounts.push_back(s);
        }
    }

    // Log the mount stack for debugging.
    if (g_mounts.empty()) {
        PrintLog(const_cast<char*>("[VFS] no mods enabled\n"));
    } else {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "[VFS] %d mod(s) mounted:\n", (int)g_mounts.size());
        PrintLog(msg);
        for (auto& m : g_mounts) {
            std::snprintf(msg, sizeof(msg), "[VFS]   %s\n", m.c_str());
            PrintLog(msg);
        }
    }
}

int MountCount() { return (int)g_mounts.size(); }

std::string ResolveRead(const char* path) {
    if (!path || !*path) return std::string();
    if (g_mounts.empty()) return std::string(path);

    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& mount : g_mounts) {
        std::string candidate = Join(mount, path);
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            return candidate;
        }
    }
    return std::string(path);
}

std::FILE* fopen(const char* path, const char* mode) {
    std::string resolved = ResolveRead(path);
    return std::fopen(resolved.c_str(), mode);
}

} // namespace VFS
