#pragma once
#include <atomic>
#include <nlohmann/json.hpp>

namespace taczlean {

class LeanModule {
public:
    static LeanModule& get();

    static constexpr const char* moduleId = "taczlean.lean";
    static constexpr const char* name = "TACZ Lean";

    void init();
    void shutdown();
    void onCameraBlend(void* cameraComponent, float dt);

    void setLeanLeft(bool held);
    void setLeanRight(bool held);

    void setEnabled(bool v) { m_enabled = v; }
    bool enabled() const { return m_enabled; }

    void loadConfig(const nlohmann::json& j);
    void saveConfig(nlohmann::json& j) const;

    // From Java ClientEvents defaults
    float m_maxLeanDeg{17.f};
    float m_smoothFactor{0.25f};
    // Java: toRadians(angle) * 55 ≈ angle * 0.96 degrees of roll
    float m_firstPersonRollMult{0.96f};

    bool m_enabled{true};
    bool m_showButtons{true};
    int m_buttonBgSize{160};
    int m_buttonIconSize{100};
    int m_buttonLeftX{120};
    int m_buttonLeftY{700};
    int m_buttonRightX{280};
    int m_buttonRightY{700};
    float m_buttonIconOpacity{0.85f};
    float m_buttonBgOpacity{0.55f};

private:
    LeanModule() = default;
    std::atomic<bool> m_left{false};
    std::atomic<bool> m_right{false};
    float m_targetAngle{0.f};
    float m_currentAngle{0.f};
    float m_prevAngle{0.f};
    bool m_hooked{false};
};

} // namespace taczlean
