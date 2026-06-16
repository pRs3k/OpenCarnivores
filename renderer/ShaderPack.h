#pragma once
// SOURCEPORT: Shader pack system — JSON-driven post-process configuration for modders.
// A pack is a directory under shaderpacks/ containing pack.json.
// pack.json declares which built-in effects to enable and what parameters to use.
// Apply() pushes only the parameters *present* in that pack's JSON to RendererGL,
// so multiple packs can be active simultaneously without resetting each other's settings.
// Active packs are listed in shaderpacks/packs.cfg (one name per line, # comments ok).

#include <string>
#include <vector>
#include <unordered_set>
#include <memory>

class RendererGL;

// All post-process parameters a shader pack can configure.
struct PostFXConfig {
    // Bloom
    bool  bloomEnabled   = false;
    float bloomThreshold = 0.78f;
    float bloomIntensity = 1.5f;

    // Tone mapping: "none" | "aces"
    std::string tonemapMode = "none";
    float       exposure    = 1.0f;

    // Color grading
    bool  cgEnabled    = false;
    float cgSaturation = 1.0f;
    float cgContrast   = 1.0f;
    float cgLift[3]    = {0.0f, 0.0f, 0.0f};
    float cgGain[3]    = {1.0f, 1.0f, 1.0f};

    // Sharpen
    float sharpenStrength = 0.0f;

    // World shadows: "none" | "full" ("dinos_only" planned, treated as "full" for now)
    std::string shadowsMode   = "none";
    float       shadowStrength = 0.5f;

    // God rays (screen-space crepuscular rays from the sun)
    bool  godRaysEnabled  = false;
    float godRayIntensity = 0.5f;
    float godRayDensity   = 0.9f;
    float godRayDecay     = 0.96f;
    float godRayColor[3]  = {1.0f, 0.92f, 0.75f};

    // Volumetric height fog (depth-aware, sun forward scattering)
    bool  heightFogEnabled  = false;
    float heightFogDensity  = 0.00012f;
    float heightFogFalloff  = 0.0003f;
    float heightFogSunPower = 8.0f;
    float heightFogColor[3]    = {0.65f, 0.72f, 0.80f};
    float heightFogSunColor[3] = {1.0f, 0.85f, 0.65f};

    // SSAO (depth-only screen-space ambient occlusion)
    bool  ssaoEnabled   = false;
    float ssaoStrength  = 0.7f;
    float ssaoRadius    = 120.0f;
    float ssaoIntensity = 1.2f;
    bool  ssaoDebug     = false;   // visualize the AO buffer (development aid)

    // Water material (GL path only; no-op on D3D and in VR/underwater)
    bool  waterEnabled      = true;
    float waterWaveStrength = 0.18f;
    float waterClarity      = 0.005f;
    float waterDeepColor[3] = {0.07f, 0.18f, 0.22f};
    float waterFoamWidth    = 50.0f;
    float waterReflectivity = 0.35f;
};

class ShaderPack {
public:
    explicit ShaderPack(const std::string& name);

    // Load and parse shaderpacks/<name>/pack.json.
    bool Load();

    // Push only the parameters explicitly present in this pack's JSON to renderer.
    void Apply(RendererGL* renderer) const;

    bool HasKey(const std::string& key) const { return m_presentKeys.count(key) > 0; }

    const std::string&  GetName()   const { return m_name; }
    const PostFXConfig& GetConfig() const { return m_config; }

private:
    std::string  m_name;
    std::string  m_packDir;
    PostFXConfig m_config;
    // SOURCEPORT: tracks which JSON keys were present so Apply() only touches
    // the renderer settings that this pack explicitly configures.
    std::unordered_set<std::string> m_presentKeys;
};

class ShaderPackManager {
public:
    static ShaderPackManager& Get();

    // Scan shaderpacks/ and populate the available list.
    void DiscoverPacks();

    // Read shaderpacks/packs.cfg and load all listed packs.
    // Falls back to "default" if packs.cfg is absent.
    void LoadActivePacks();

    // Apply all loaded active packs to renderer in order (each pack is sparse
    // so earlier packs' settings are not reset by later ones unless overridden).
    void ApplyAll(RendererGL* renderer);

    // Legacy single-pack helper (still usable for scripted reload).
    bool ApplyPack(const std::string& name, RendererGL* renderer);

    const std::vector<std::string>& GetAvailable()    const { return m_available; }
    const std::vector<std::string>& GetActiveNames()  const { return m_activeNames; }

private:
    ShaderPackManager() = default;
    std::vector<std::string> m_available;
    std::vector<std::string> m_activeNames;
    std::vector<ShaderPack>  m_loadedPacks;
};
