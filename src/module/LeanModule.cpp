#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uintptr_t kRotationOffset = 0x28;
// Camera component layout (CO family): position often before rotation quat.
// Try Vec3 at 0x0 — if wrong, lateral still soft-fails (rotation-only still works).
constexpr std::uintptr_t kPositionOffset = 0x0;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };

Quat quatFromRollDeg(float rollDeg) {
    const float h = rollDeg * 0.01745329251f * 0.5f;
    return {0.f, 0.f, std::sin(h), std::cos(h)};
}

Quat quatMul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Camera local +X (right) from quaternion (x,y,z,w)
Vec3 rightFromQuat(const Quat& q) {
    // right = (1-2(y^2+z^2), 2(xy+wz), 2(xz-wy)) for standard rotation quat
    return {
        1.f - 2.f * (q.y * q.y + q.z * q.z),
        2.f * (q.x * q.y + q.w * q.z),
        2.f * (q.x * q.z - q.w * q.y),
    };
}

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blend, float factor) {
    if (g_orig) g_orig(component, blend, factor);
    taczlean::LeanModule::get().onCameraBlend(component, 1.f / 60.f);
}

} // namespace

namespace taczlean {

LeanModule& LeanModule::get() {
    static LeanModule inst;
    return inst;
}

void LeanModule::init() {
    if (m_hooked) return;
    const auto addr = cameraoverhaul::memory::resolve(
        cameraoverhaul::memory::SignatureId::CameraBlendSystemTick);
    if (!addr) return;
    if (cameraoverhaul::hooks::install(reinterpret_cast<void*>(addr),
                                       reinterpret_cast<void*>(cameraBlendHook),
                                       reinterpret_cast<void**>(&g_orig))) {
        m_hooked = true;
    }
}

void LeanModule::shutdown() {
    m_left = false;
    m_right = false;
    m_toggleLeft = m_toggleRight = false;
    m_targetAngle = m_currentAngle = 0.f;
    m_targetLateral = m_currentLateral = 0.f;
}

void LeanModule::setLeanLeft(bool held) { m_left = held; }
void LeanModule::setLeanRight(bool held) { m_right = held; }

void LeanModule::onButtonLeft(bool down) {
    if (m_holdMode) {
        setLeanLeft(down);
        if (down) setLeanRight(false);
    } else if (down) {
        // toggle
        m_toggleLeft = !m_toggleLeft;
        if (m_toggleLeft) m_toggleRight = false;
        setLeanLeft(m_toggleLeft);
        setLeanRight(false);
    }
}

void LeanModule::onButtonRight(bool down) {
    if (m_holdMode) {
        setLeanRight(down);
        if (down) setLeanLeft(false);
    } else if (down) {
        m_toggleRight = !m_toggleRight;
        if (m_toggleRight) m_toggleLeft = false;
        setLeanRight(m_toggleRight);
        setLeanLeft(false);
    }
}

void LeanModule::updateTargets() {
    // Angle: negative = lean left, positive = lean right
    float target = 0.f;
    float lat = 0.f;
    if (m_left.load() && !m_right.load()) {
        target = -m_maxLeanDeg;
        lat = -m_lateralOffset; // shift camera left
    } else if (m_right.load() && !m_left.load()) {
        target = m_maxLeanDeg;
        lat = m_lateralOffset;
    }
    m_targetAngle = target;
    m_targetLateral = lat;
}

void LeanModule::onCameraBlend(void* cameraComponent, float /*dt*/) {
    if (!m_enabled || !cameraComponent) return;

    updateTargets();

    const float s = std::clamp(m_smoothFactor, 0.05f, 1.f);
    m_currentAngle += (m_targetAngle - m_currentAngle) * s;
    m_currentLateral += (m_targetLateral - m_currentLateral) * s;

    if (std::fabs(m_currentAngle) < 0.01f && std::fabs(m_targetAngle) < 0.01f)
        m_currentAngle = 0.f;
    if (std::fabs(m_currentLateral) < 0.0005f && std::fabs(m_targetLateral) < 0.0005f)
        m_currentLateral = 0.f;

    auto* base = reinterpret_cast<char*>(cameraComponent);
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    if (!(qlen > 0.5f && qlen < 1.5f)) return;

    // --- Lateral eye offset (peek around cover) ---
    if (m_enableLateral && std::fabs(m_currentLateral) > 0.0005f) {
        auto* pos = reinterpret_cast<float*>(base + kPositionOffset);
        // sanity: position should be finite world-ish coords
        if (std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(pos[2]) &&
            std::fabs(pos[0]) < 1e7f && std::fabs(pos[2]) < 1e7f) {
            Vec3 right = rightFromQuat(cur);
            // horizontal only
            right.y = 0.f;
            const float rl = std::sqrt(right.x * right.x + right.z * right.z);
            if (rl > 1e-4f) {
                right.x /= rl;
                right.z /= rl;
                pos[0] += right.x * m_currentLateral;
                pos[2] += right.z * m_currentLateral;
            }
        }
    }

    // --- Roll ---
    if (m_enableRoll && std::fabs(m_currentAngle) > 0.01f) {
        const float rollDeg = m_currentAngle * m_firstPersonRollMult;
        Quat bias = quatFromRollDeg(rollDeg);
        Quat out = quatMul(cur, bias);
        const float n = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
        if (n > 1e-6f) {
            qf[0] = out.x / n;
            qf[1] = out.y / n;
            qf[2] = out.z / n;
            qf[3] = out.w / n;
        }
    }
}

void LeanModule::loadConfig(const nlohmann::json& j) {
    auto gf = [&](const char* k, float& v) {
        if (j.contains(k) && j[k].is_number()) v = j[k].get<float>();
    };
    auto gb = [&](const char* k, bool& v) {
        if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>();
    };
    gb("enabled", m_enabled);
    gf("maxLeanDeg", m_maxLeanDeg);
    gf("smoothFactor", m_smoothFactor);
    gf("firstPersonRollMult", m_firstPersonRollMult);
    gf("lateralOffset", m_lateralOffset);
    gb("enableRoll", m_enableRoll);
    gb("enableLateral", m_enableLateral);
    gb("holdMode", m_holdMode);
    gb("showButtons", m_showButtons);
}

void LeanModule::saveConfig(nlohmann::json& j) const {
    j["enabled"] = m_enabled;
    j["maxLeanDeg"] = m_maxLeanDeg;
    j["smoothFactor"] = m_smoothFactor;
    j["firstPersonRollMult"] = m_firstPersonRollMult;
    j["lateralOffset"] = m_lateralOffset;
    j["enableRoll"] = m_enableRoll;
    j["enableLateral"] = m_enableLateral;
    j["holdMode"] = m_holdMode;
    j["showButtons"] = m_showButtons;
}

} // namespace taczlean
