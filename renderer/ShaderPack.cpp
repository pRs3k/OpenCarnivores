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
    m_presentKeys.clear();
    std::string path = m_packDir + "/pack.json";
    std::string json = ReadFile(path);
    if (json.empty()) {
        fprintf(stderr, "[ShaderPack] Cannot read %s\n", path.c_str());
        return false;
    }

    // SOURCEPORT: track which keys are present so Apply() can be sparse —
    // a pack that only sets water parameters must not reset bloom/SSAO/etc.
    auto track = [&](const std::string& key) -> bool {
        bool found = !FindValue(json, key).empty();
        if (found) m_presentKeys.insert(key);
        return found;
    };

    // Bloom
    track("bloom_enabled");
    m_config.bloomEnabled   = JsonBool  (json, "bloom_enabled",   false);
    track("bloom_threshold");
    m_config.bloomThreshold = JsonFloat (json, "bloom_threshold", 0.78f);
    track("bloom_intensity");
    m_config.bloomIntensity = JsonFloat (json, "bloom_intensity", 1.5f);

    // Tone mapping
    track("tonemap_mode");
    m_config.tonemapMode = JsonString(json, "tonemap_mode", "none");
    track("exposure");
    m_config.exposure    = JsonFloat (json, "exposure",     1.0f);

    // Color grading
    track("colorgrade_enabled");
    m_config.cgEnabled    = JsonBool (json, "colorgrade_enabled",    false);
    track("colorgrade_saturation");
    m_config.cgSaturation = JsonFloat(json, "colorgrade_saturation", 1.0f);
    track("colorgrade_contrast");
    m_config.cgContrast   = JsonFloat(json, "colorgrade_contrast",   1.0f);
    track("colorgrade_lift_r");
    m_config.cgLift[0]    = JsonFloat(json, "colorgrade_lift_r",     0.0f);
    track("colorgrade_lift_g");
    m_config.cgLift[1]    = JsonFloat(json, "colorgrade_lift_g",     0.0f);
    track("colorgrade_lift_b");
    m_config.cgLift[2]    = JsonFloat(json, "colorgrade_lift_b",     0.0f);
    track("colorgrade_gain_r");
    m_config.cgGain[0]    = JsonFloat(json, "colorgrade_gain_r",     1.0f);
    track("colorgrade_gain_g");
    m_config.cgGain[1]    = JsonFloat(json, "colorgrade_gain_g",     1.0f);
    track("colorgrade_gain_b");
    m_config.cgGain[2]    = JsonFloat(json, "colorgrade_gain_b",     1.0f);

    // Sharpen
    track("sharpen_strength");
    m_config.sharpenStrength = JsonFloat(json, "sharpen_strength", 0.0f);

    // World shadows
    track("shadows_mode");
    m_config.shadowsMode    = JsonString(json, "shadows_mode",   "none");
    track("shadow_strength");
    m_config.shadowStrength = JsonFloat (json, "shadow_strength", 0.5f);

    // God rays
    track("godrays_enabled");
    m_config.godRaysEnabled  = JsonBool (json, "godrays_enabled",   false);
    track("godrays_intensity");
    m_config.godRayIntensity = JsonFloat(json, "godrays_intensity", 0.5f);
    track("godrays_density");
    m_config.godRayDensity   = JsonFloat(json, "godrays_density",   0.9f);
    track("godrays_decay");
    m_config.godRayDecay     = JsonFloat(json, "godrays_decay",     0.96f);
    track("godrays_color_r");
    m_config.godRayColor[0]  = JsonFloat(json, "godrays_color_r",   1.0f);
    track("godrays_color_g");
    m_config.godRayColor[1]  = JsonFloat(json, "godrays_color_g",   0.92f);
    track("godrays_color_b");
    m_config.godRayColor[2]  = JsonFloat(json, "godrays_color_b",   0.75f);

    // Height fog
    track("heightfog_enabled");
    m_config.heightFogEnabled  = JsonBool (json, "heightfog_enabled",   false);
    track("heightfog_density");
    m_config.heightFogDensity  = JsonFloat(json, "heightfog_density",   0.00012f);
    track("heightfog_falloff");
    m_config.heightFogFalloff  = JsonFloat(json, "heightfog_falloff",   0.0003f);
    track("heightfog_sun_power");
    m_config.heightFogSunPower = JsonFloat(json, "heightfog_sun_power", 8.0f);
    track("heightfog_color_r");
    m_config.heightFogColor[0] = JsonFloat(json, "heightfog_color_r",   0.65f);
    track("heightfog_color_g");
    m_config.heightFogColor[1] = JsonFloat(json, "heightfog_color_g",   0.72f);
    track("heightfog_color_b");
    m_config.heightFogColor[2] = JsonFloat(json, "heightfog_color_b",   0.80f);
    track("heightfog_suncolor_r");
    m_config.heightFogSunColor[0] = JsonFloat(json, "heightfog_suncolor_r", 1.0f);
    track("heightfog_suncolor_g");
    m_config.heightFogSunColor[1] = JsonFloat(json, "heightfog_suncolor_g", 0.85f);
    track("heightfog_suncolor_b");
    m_config.heightFogSunColor[2] = JsonFloat(json, "heightfog_suncolor_b", 0.65f);

    // SSAO
    track("ssao_enabled");
    m_config.ssaoEnabled   = JsonBool (json, "ssao_enabled",   false);
    track("ssao_strength");
    m_config.ssaoStrength  = JsonFloat(json, "ssao_strength",  0.7f);
    track("ssao_radius");
    m_config.ssaoRadius    = JsonFloat(json, "ssao_radius",    120.0f);
    track("ssao_intensity");
    m_config.ssaoIntensity = JsonFloat(json, "ssao_intensity", 1.2f);
    track("ssao_debug");
    m_config.ssaoDebug     = JsonBool (json, "ssao_debug",     false);

    // Water material
    track("water_enabled");
    m_config.waterEnabled      = JsonBool (json, "water_enabled",       true);
    track("water_wave_strength");
    m_config.waterWaveStrength = JsonFloat(json, "water_wave_strength", 0.18f);
    track("water_clarity");
    m_config.waterClarity      = JsonFloat(json, "water_clarity",       0.005f);
    track("water_deep_color_r");
    m_config.waterDeepColor[0] = JsonFloat(json, "water_deep_color_r",  0.07f);
    track("water_deep_color_g");
    m_config.waterDeepColor[1] = JsonFloat(json, "water_deep_color_g",  0.18f);
    track("water_deep_color_b");
    m_config.waterDeepColor[2] = JsonFloat(json, "water_deep_color_b",  0.22f);
    track("water_foam_width");
    m_config.waterFoamWidth    = JsonFloat(json, "water_foam_width",    50.0f);
    track("water_reflectivity");
    m_config.waterReflectivity = JsonFloat(json, "water_reflectivity",  0.35f);

    fprintf(stdout, "[ShaderPack] Loaded '%s' (%zu key(s) set)\n",
            m_name.c_str(), m_presentKeys.size());
    return true;
}

void ShaderPack::Apply(RendererGL* renderer) const {
    if (!renderer) return;

    // SOURCEPORT: each setter is gated on the key being present in this pack's
    // JSON.  Absent keys leave the renderer's current value untouched, so
    // multiple packs can be active simultaneously without stepping on each other.
    auto has = [&](const std::string& key) -> bool {
        return m_presentKeys.count(key) > 0;
    };
    auto hasAny = [&](std::initializer_list<const char*> keys) -> bool {
        for (const char* k : keys) if (m_presentKeys.count(k)) return true;
        return false;
    };

    // Bloom
    if (has("bloom_enabled"))   renderer->EnablePostOverlay(m_config.bloomEnabled);
    if (has("bloom_threshold")) renderer->SetBloomThreshold(m_config.bloomThreshold);
    if (has("bloom_intensity")) renderer->SetBloomIntensity(m_config.bloomIntensity);

    // Tone mapping
    if (has("tonemap_mode")) {
        int tmMode = 0;
        if      (m_config.tonemapMode == "aces")     tmMode = 1;
        else if (m_config.tonemapMode == "reinhard") tmMode = 2;
        renderer->SetToneMappingMode(tmMode);
    }
    if (has("exposure")) renderer->SetExposure(m_config.exposure);

    // Color grading
    if (has("colorgrade_enabled"))    renderer->SetColorGradingEnabled(m_config.cgEnabled);
    if (has("colorgrade_saturation")) renderer->SetCGSaturation(m_config.cgSaturation);
    if (has("colorgrade_contrast"))   renderer->SetCGContrast(m_config.cgContrast);
    if (hasAny({"colorgrade_lift_r","colorgrade_lift_g","colorgrade_lift_b"}))
        renderer->SetCGLift(m_config.cgLift[0], m_config.cgLift[1], m_config.cgLift[2]);
    if (hasAny({"colorgrade_gain_r","colorgrade_gain_g","colorgrade_gain_b"}))
        renderer->SetCGGain(m_config.cgGain[0], m_config.cgGain[1], m_config.cgGain[2]);

    // Sharpen
    if (has("sharpen_strength")) renderer->SetSharpenStrength(m_config.sharpenStrength);

    // World shadows  (0=none, 2=full; "dinos_only" reserved → mapped to 2 for now)
    if (has("shadows_mode")) {
        int smode = 0;
        if (m_config.shadowsMode == "full" || m_config.shadowsMode == "dinos_only") smode = 2;
        renderer->SetShadowMode(smode);
    }
    if (has("shadow_strength")) renderer->SetShadowStrength(m_config.shadowStrength);

    // God rays
    if (has("godrays_enabled"))   renderer->SetGodRaysEnabled(m_config.godRaysEnabled);
    if (has("godrays_intensity")) renderer->SetGodRayIntensity(m_config.godRayIntensity);
    if (has("godrays_density"))   renderer->SetGodRayDensity(m_config.godRayDensity);
    if (has("godrays_decay"))     renderer->SetGodRayDecay(m_config.godRayDecay);
    if (hasAny({"godrays_color_r","godrays_color_g","godrays_color_b"}))
        renderer->SetGodRayColor(m_config.godRayColor[0], m_config.godRayColor[1], m_config.godRayColor[2]);

    // Height fog
    if (has("heightfog_enabled"))   renderer->SetHeightFogEnabled(m_config.heightFogEnabled);
    if (has("heightfog_density"))   renderer->SetHeightFogDensity(m_config.heightFogDensity);
    if (has("heightfog_falloff"))   renderer->SetHeightFogFalloff(m_config.heightFogFalloff);
    if (has("heightfog_sun_power")) renderer->SetHeightFogSunPower(m_config.heightFogSunPower);
    if (hasAny({"heightfog_color_r","heightfog_color_g","heightfog_color_b"}))
        renderer->SetHeightFogColor(m_config.heightFogColor[0], m_config.heightFogColor[1], m_config.heightFogColor[2]);
    if (hasAny({"heightfog_suncolor_r","heightfog_suncolor_g","heightfog_suncolor_b"}))
        renderer->SetHeightFogSunColor(m_config.heightFogSunColor[0], m_config.heightFogSunColor[1], m_config.heightFogSunColor[2]);

    // SSAO
    if (has("ssao_enabled"))   renderer->SetSSAOEnabled(m_config.ssaoEnabled);
    if (has("ssao_strength"))  renderer->SetSSAOStrength(m_config.ssaoStrength);
    if (has("ssao_radius"))    renderer->SetSSAORadius(m_config.ssaoRadius);
    if (has("ssao_intensity")) renderer->SetSSAOIntensity(m_config.ssaoIntensity);
    if (has("ssao_debug"))     renderer->SetSSAODebug(m_config.ssaoDebug);

    // Water material
    if (has("water_enabled"))       renderer->SetWaterFXEnabled(m_config.waterEnabled);
    if (has("water_wave_strength")) renderer->SetWaterWaveStrength(m_config.waterWaveStrength);
    if (has("water_clarity"))       renderer->SetWaterClarity(m_config.waterClarity);
    if (hasAny({"water_deep_color_r","water_deep_color_g","water_deep_color_b"}))
        renderer->SetWaterDeepColor(m_config.waterDeepColor[0], m_config.waterDeepColor[1], m_config.waterDeepColor[2]);
    if (has("water_foam_width"))    renderer->SetWaterFoamWidth(m_config.waterFoamWidth);
    if (has("water_reflectivity"))  renderer->SetWaterReflectivity(m_config.waterReflectivity);
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
                if (fs::exists(entry.path() / "pack.json"))
                    m_available.push_back(name);
            }
        }
        fprintf(stdout, "[ShaderPackManager] Found %zu pack(s)\n", m_available.size());
    } catch (const std::exception& e) {
        fprintf(stderr, "[ShaderPackManager] Discovery error: %s\n", e.what());
    }
}

void ShaderPackManager::LoadActivePacks() {
    m_activeNames.clear();
    m_loadedPacks.clear();

    // SOURCEPORT: read shaderpacks/packs.cfg — one pack name per line.
    // Lines starting with # (after optional leading whitespace) are comments.
    // Falls back to "default" if packs.cfg is absent, for backward compatibility.
    std::string cfgPath = "shaderpacks/packs.cfg";
    std::ifstream f(cfgPath);
    if (!f) {
        fprintf(stdout, "[ShaderPackManager] No packs.cfg — falling back to 'default'\n");
        for (const auto& n : m_available) {
            if (n == "default") {
                m_activeNames.push_back("default");
                m_loadedPacks.emplace_back("default");
                m_loadedPacks.back().Load();
            }
        }
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        if (line.empty() || line[0] == '#') continue;
        // Strip inline comment and trailing whitespace
        size_t end = line.find_first_of(" \t#");
        if (end != std::string::npos) line = line.substr(0, end);
        if (line.empty()) continue;

        m_activeNames.push_back(line);
        m_loadedPacks.emplace_back(line);
        if (!m_loadedPacks.back().Load()) {
            fprintf(stderr, "[ShaderPackManager] Failed to load pack '%s' — skipping\n", line.c_str());
            m_activeNames.pop_back();
            m_loadedPacks.pop_back();
        }
    }

    fprintf(stdout, "[ShaderPackManager] Applied %zu active pack(s) from packs.cfg\n",
            m_activeNames.size());
}

void ShaderPackManager::ApplyAll(RendererGL* renderer) {
    for (const auto& pack : m_loadedPacks)
        pack.Apply(renderer);
}

bool ShaderPackManager::ApplyPack(const std::string& name, RendererGL* renderer) {
    ShaderPack pack(name);
    if (!pack.Load()) return false;
    pack.Apply(renderer);
    fprintf(stdout, "[ShaderPackManager] Applied pack '%s'\n", name.c_str());
    return true;
}
