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
    if (v.empty()) return def;
    // SOURCEPORT: strtof instead of atof — atof returns 0.0 on malformed input
    // with no error signal; fall back to the documented default instead.
    char* end = nullptr;
    float r = std::strtof(v.c_str(), &end);
    return (end == v.c_str()) ? def : r;
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

    // God rays
    m_config.godRaysEnabled  = JsonBool (json, "godrays_enabled",   false);
    m_config.godRayIntensity = JsonFloat(json, "godrays_intensity", 0.5f);
    m_config.godRayDensity   = JsonFloat(json, "godrays_density",   0.9f);
    m_config.godRayDecay     = JsonFloat(json, "godrays_decay",     0.96f);
    m_config.godRayColor[0]  = JsonFloat(json, "godrays_color_r",   1.0f);
    m_config.godRayColor[1]  = JsonFloat(json, "godrays_color_g",   0.92f);
    m_config.godRayColor[2]  = JsonFloat(json, "godrays_color_b",   0.75f);

    // Height fog
    m_config.heightFogEnabled  = JsonBool (json, "heightfog_enabled",   false);
    m_config.heightFogDensity  = JsonFloat(json, "heightfog_density",   0.00012f);
    m_config.heightFogFalloff  = JsonFloat(json, "heightfog_falloff",   0.0003f);
    m_config.heightFogSunPower = JsonFloat(json, "heightfog_sun_power", 8.0f);
    m_config.heightFogColor[0] = JsonFloat(json, "heightfog_color_r",   0.65f);
    m_config.heightFogColor[1] = JsonFloat(json, "heightfog_color_g",   0.72f);
    m_config.heightFogColor[2] = JsonFloat(json, "heightfog_color_b",   0.80f);
    m_config.heightFogSunColor[0] = JsonFloat(json, "heightfog_suncolor_r", 1.0f);
    m_config.heightFogSunColor[1] = JsonFloat(json, "heightfog_suncolor_g", 0.85f);
    m_config.heightFogSunColor[2] = JsonFloat(json, "heightfog_suncolor_b", 0.65f);

    // SSAO
    m_config.ssaoEnabled   = JsonBool (json, "ssao_enabled",   false);
    m_config.ssaoStrength  = JsonFloat(json, "ssao_strength",  0.7f);
    m_config.ssaoRadius    = JsonFloat(json, "ssao_radius",    120.0f);
    m_config.ssaoIntensity = JsonFloat(json, "ssao_intensity", 1.2f);
    m_config.ssaoDebug     = JsonBool (json, "ssao_debug",     false);

    // Water material
    m_config.waterEnabled      = JsonBool (json, "water_enabled",       true);
    m_config.waterWaveStrength = JsonFloat(json, "water_wave_strength", 0.35f);
    m_config.waterClarity      = JsonFloat(json, "water_clarity",       0.0025f);
    m_config.waterDeepColor[0] = JsonFloat(json, "water_deep_color_r",  0.07f);
    m_config.waterDeepColor[1] = JsonFloat(json, "water_deep_color_g",  0.18f);
    m_config.waterDeepColor[2] = JsonFloat(json, "water_deep_color_b",  0.22f);
    m_config.waterFoamWidth    = JsonFloat(json, "water_foam_width",    50.0f);
    m_config.waterReflectivity = JsonFloat(json, "water_reflectivity",  0.85f);

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

    // God rays
    renderer->SetGodRaysEnabled(m_config.godRaysEnabled);
    renderer->SetGodRayIntensity(m_config.godRayIntensity);
    renderer->SetGodRayDensity(m_config.godRayDensity);
    renderer->SetGodRayDecay(m_config.godRayDecay);
    renderer->SetGodRayColor(m_config.godRayColor[0], m_config.godRayColor[1], m_config.godRayColor[2]);

    // Height fog
    renderer->SetHeightFogEnabled(m_config.heightFogEnabled);
    renderer->SetHeightFogDensity(m_config.heightFogDensity);
    renderer->SetHeightFogFalloff(m_config.heightFogFalloff);
    renderer->SetHeightFogSunPower(m_config.heightFogSunPower);
    renderer->SetHeightFogColor(m_config.heightFogColor[0], m_config.heightFogColor[1], m_config.heightFogColor[2]);
    renderer->SetHeightFogSunColor(m_config.heightFogSunColor[0], m_config.heightFogSunColor[1], m_config.heightFogSunColor[2]);

    // SSAO
    renderer->SetSSAOEnabled(m_config.ssaoEnabled);
    renderer->SetSSAOStrength(m_config.ssaoStrength);
    renderer->SetSSAORadius(m_config.ssaoRadius);
    renderer->SetSSAOIntensity(m_config.ssaoIntensity);
    renderer->SetSSAODebug(m_config.ssaoDebug);

    // Water material
    renderer->SetWaterFXEnabled(m_config.waterEnabled);
    renderer->SetWaterWaveStrength(m_config.waterWaveStrength);
    renderer->SetWaterClarity(m_config.waterClarity);
    renderer->SetWaterDeepColor(m_config.waterDeepColor[0], m_config.waterDeepColor[1], m_config.waterDeepColor[2]);
    renderer->SetWaterFoamWidth(m_config.waterFoamWidth);
    renderer->SetWaterReflectivity(m_config.waterReflectivity);
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
