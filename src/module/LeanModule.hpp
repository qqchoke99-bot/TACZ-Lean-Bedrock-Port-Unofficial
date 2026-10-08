#pragma once
#include <atomic>
#include <nlohmann/json.hpp>
#include <string>

namespace taczlean {

class LeanModule {
public:
    static LeanModule& get();

    static constexpr const char* moduleId = "taczlean.lean";
    static constexpr const char* name = "TACZ Lean";

    void init();
    void shutdown();
    void onCameraBlend(void* cameraComponent, float dt);
    void applyLateralOnly(void* anyCameraObj);

    void setLeanLeft(bool held);
    void setLeanRight(bool held);
    void onButtonLeft(bool down);
    void onButtonRight(bool down);

    void setEnabled(bool v) { m_enabled = v; }
    bool enabled() const { return m_enabled; }

    void loadConfig(const nlohmann::json& j);
    void saveConfig(nlohmann::json& j) const;

    // Settings (exposed via Mod Menu)
    float m_maxLeanDeg{17.f};          // max lean angle (degrees)
    float m_smoothFactor{0.25f};       // 0..1 lerp per frame
    float m_firstPersonRollMult{0.85f}; // roll strength relative to angle
    float m_lateralOffset{0.22f};      // blocks — eye shift left/right (peek)
    bool  m_enableRoll{true};
    bool  m_enableLateral{true};
    bool  m_holdMode{true};            // true=hold, false=toggle
    bool  m_enabled{true};
    bool  m_showButtons{true};

private:
    LeanModule() = default;
    std::atomic<bool> m_left{false};
    std::atomic<bool> m_right{false};
    // toggle latches
    bool m_toggleLeft{false};
    bool m_toggleRight{false};

    float m_targetAngle{0.f};
    float m_currentAngle{0.f};
    float m_targetLateral{0.f};
    float m_currentLateral{0.f};
    bool m_hooked{false};

    void updateTargets();
};

} // namespace taczlean
