#include "ShaderPack.h"
#include "RendererGL.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fs = std::filesystem;

// ── Minimal flat-JSON helpers ─────────────────────────────────────────────────
// Only handles top-level key/value pairs (no nested objects, no arrays with
// >1 element). Sufficient for pack.json which is intentionally flat.

static std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

// Find the value string that follows "key": in json.
// Returns empty string if key not found.
static std::string FindValue(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                  json[pos] == '\r' || json[pos] == '\n'))
        ++pos;
    // Collect until comma, closing brace, or end
    size_t end = pos;
    bool inStr = (json[pos] == '"');
    if (inStr) {
        ++end;
        while (end < json.size() && json[end] != '"') ++end;
        return json.substr(pos + 1, end - pos - 1);
    }
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != '\n' && json[end] != '\r')
        ++end;
    std::string val = json.substr(pos, end - pos);
    // trim trailing whitespace
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
    return val;
}

static float   JsonFloat(const std::string& json, const std::string& key, float   def) {
    auto v = FindValue(json, key);
    return v.empty() ? def : (float)std::atof(v.c_str());
}
static bool    JsonBool(const std::string& json, const std::string& key, bool    def) {
    auto v = FindValue(json, key);
    if (v.empty()) return def;
    return v == "true" || v == "1";
}
static std::string JsonString(const std::string& json, const std::string& key,
                               const std::string& def) {
    auto v = FindValue(json, key);
    return v.empty() ? def : v;
}

// ── ShaderPack ────────────────────────────────────────────────────────────────

ShaderPack::ShaderPack(const std::string& name) : m_name(name) {
    m_packDir = "shaderpacks/" + name;
}

bool ShaderPack::Load() {
    std::string path = m_packDir + "/pack.json";
    std::string json = ReadFile(path);
    if (json.empty()) {
        fprintf(stderr, "[ShaderPack] Cannot read %s\n", path.c_str());
        return false;
    }

    // Bloom
    m_config.bloomEnabled   = JsonBool  (json, "bloom_enabled",   false);
    m_config.bloomThreshold = JsonFloat (json, "bloom_threshold", 0.78f);
    m_config.bloomIntensity = JsonFloat (json, "bloom_intensity", 1.5f);

    // Tone mapping
    m_config.tonemapMode = JsonString(json, "tonemap_mode", "none");
    m_config.exposure    = JsonFloat (json, "exposure",     1.0f);

    // Color grading
    m_config.cgEnabled    = JsonBool (json, "colorgrade_enabled",    false);
    m_config.cgSaturation = JsonFloat(json, "colorgrade_saturation", 1.0f);
    m_config.cgContrast   = JsonFloat(json, "colorgrade_contrast",   1.0f);
    m_config.cgLift[0]    = JsonFloat(json, "colorgrade_lift_r",     0.0f);
    m_config.cgLift[1]    = JsonFloat(json, "colorgrade_lift_g",     0.0f);
    m_config.cgLift[2]    = JsonFloat(json, "colorgrade_lift_b",     0.0f);
    m_config.cgGain[0]    = JsonFloat(json, "colorgrade_gain_r",     1.0f);
    m_config.cgGain[1]    = JsonFloat(json, "colorgrade_gain_g",     1.0f);
    m_config.cgGain[2]    = JsonFloat(json, "colorgrade_gain_b",     1.0f);

    // Sharpen
    m_config.sharpenStrength = JsonFloat(json, "sharpen_strength", 0.0f);

    // World shadows
    m_config.shadowsMode    = JsonString(json, "shadows_mode",   "none");
    m_config.shadowStrength = JsonFloat (json, "shadow_strength", 0.5f);

    fprintf(stdout, "[ShaderPack] Loaded '%s' — bloom=%s tonemap=%s cg=%s shadows=%s\n",
            m_name.c_str(),
            m_config.bloomEnabled ? "on" : "off",
            m_config.tonemapMode.c_str(),
            m_config.cgEnabled    ? "on" : "off",
            m_config.shadowsMode.c_str());
    return true;
}

void ShaderPack::Apply(RendererGL* renderer) const {
    if (!renderer) return;

    // Bloom
    renderer->EnablePostOverlay(m_config.bloomEnabled);
    renderer->SetBloomThreshold(m_config.bloomThreshold);
    renderer->SetBloomIntensity(m_config.bloomIntensity);

    // Tone mapping
    int tmMode = 0;
    if      (m_config.tonemapMode == "aces")     tmMode = 1;
    else if (m_config.tonemapMode == "reinhard") tmMode = 2;
    renderer->SetToneMappingMode(tmMode);
    renderer->SetExposure(m_config.exposure);

    // Color grading
    renderer->SetColorGradingEnabled(m_config.cgEnabled);
    renderer->SetCGSaturation(m_config.cgSaturation);
    renderer->SetCGContrast(m_config.cgContrast);
    renderer->SetCGLift(m_config.cgLift[0], m_config.cgLift[1], m_config.cgLift[2]);
    renderer->SetCGGain(m_config.cgGain[0], m_config.cgGain[1], m_config.cgGain[2]);

    // Sharpen
    renderer->SetSharpenStrength(m_config.sharpenStrength);

    // World shadows  (0=none, 2=full; "dinos_only" reserved → mapped to 2 for now)
    int smode = 0;
    if (m_config.shadowsMode == "full" || m_config.shadowsMode == "dinos_only") smode = 2;
    renderer->SetShadowMode(smode);
    renderer->SetShadowStrength(m_config.shadowStrength);
}

// ── ShaderPackManager ─────────────────────────────────────────────────────────

ShaderPackManager& ShaderPackManager::Get() {
    static ShaderPackManager instance;
    return instance;
}

void ShaderPackManager::DiscoverPacks() {
    m_available.clear();
    try {
        if (!fs::exists("shaderpacks")) fs::create_directory("shaderpacks");
        for (const auto& entry : fs::directory_iterator("shaderpacks")) {
            if (entry.is_directory()) {
                std::string name = entry.path().filename().string();
                // Only list directories that contain a pack.json
                if (fs::exists(entry.path() / "pack.json"))
                    m_available.push_back(name);
            }
        }
        fprintf(stdout, "[ShaderPackManager] Found %zu pack(s)\n", m_available.size());
    } catch (const std::exception& e) {
        fprintf(stderr, "[ShaderPackManager] Discovery error: %s\n", e.what());
    }
}

bool ShaderPackManager::ApplyPack(const std::string& name, RendererGL* renderer) {
    ShaderPack pack(name);
    if (!pack.Load()) return false;
    pack.Apply(renderer);
    m_activeName = name;
    fprintf(stdout, "[ShaderPackManager] Applied pack '%s'\n", name.c_str());
    return true;
}
