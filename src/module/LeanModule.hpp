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

    // Body-part lean: shares the camera smoothing state, each part's angle is
    // maxLeanDeg * its scale (1.0 = same angle for every part).
    bool  bodyLeanEnabled() const { return m_bodyLeanEnabled; }
    float headLeanDeg() const { return m_currentAngle * m_headScale; }
    float bodyLeanDeg() const { return m_currentAngle * m_bodyScale; }
    float armLeanDeg() const { return m_currentAngle * m_armScale; }

    // Per-tick driver invoked by the NormalTick forwarding hook.
    void onBodyTick();

    // Probe: logs humanoid part registrations (owner object + memory dump).
    void onRegisterPart(void* owner, void* pair, const char* name,
                        unsigned long long len, void* result);

    void loadConfig(const nlohmann::json& j);
    void saveConfig(nlohmann::json& j) const;

    // Settings (exposed via Mod Menu)
    float m_maxLeanDeg{17.f};          // max lean angle (degrees)
    float m_smoothFactor{0.25f};       // 0..1 lerp per frame
    float m_firstPersonRollMult{0.85f}; // roll strength relative to angle
    float m_lateralOffset{0.40f};      // blocks — eye shift left/right (peek)
    bool  m_enableRoll{true};
    bool  m_enableLateral{true};
    bool  m_holdMode{true};            // true=hold, false=toggle
    bool  m_enabled{true};
    bool  m_showButtons{true};
    bool  m_bodyLeanEnabled{true};     // body-part lean master toggle
    float m_headScale{1.f};            // part angle = maxLeanDeg * scale
    float m_bodyScale{1.f};            // torso
    float m_armScale{1.f};             // both arms

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
    bool m_tickHooked{false};
    bool m_partHooked{false};
    // log throttling (only touched from the camera-blend thread)
    bool m_bodyLogRequested{false};
    int  m_bodyLogCooldown{0};

    void updateTargets();
    void logBodyState();
};

} // namespace taczlean
