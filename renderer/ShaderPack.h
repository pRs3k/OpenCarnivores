#pragma once
// SOURCEPORT: Shader pack system — JSON-driven post-process configuration for modders.
// A pack is a directory under shaderpacks/ containing pack.json.
// pack.json declares which built-in effects to enable and what parameters to use.
// Apply() pushes those parameters to RendererGL via its public setters.

#include <string>
#include <vector>
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
};

class ShaderPack {
public:
    explicit ShaderPack(const std::string& name);

    // Load and parse shaderpacks/<name>/pack.json.
    bool Load();

    // Push config to renderer.
    void Apply(RendererGL* renderer) const;

    const std::string&  GetName()   const { return m_name; }
    const PostFXConfig& GetConfig() const { return m_config; }

private:
    std::string  m_name;
    std::string  m_packDir;
    PostFXConfig m_config;
};

class ShaderPackManager {
public:
    static ShaderPackManager& Get();

    // Scan shaderpacks/ and populate available list.
    void DiscoverPacks();

    // Load pack by name and immediately apply it to renderer.
    // Returns false if pack.json is missing or malformed.
    bool ApplyPack(const std::string& name, RendererGL* renderer);

    const std::vector<std::string>& GetAvailable() const { return m_available; }
    const std::string& GetActiveName() const { return m_activeName; }

private:
    ShaderPackManager() = default;
    std::vector<std::string> m_available;
    std::string              m_activeName;
};
