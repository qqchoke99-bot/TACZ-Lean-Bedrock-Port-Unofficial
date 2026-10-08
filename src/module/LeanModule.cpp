#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uintptr_t kRotationOffset = 0x28;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };

Quat quatFromEulerDeg(float pitch, float yaw, float roll) {
    const float d = 0.01745329251f * 0.5f;
    const float p = pitch * d, y = yaw * d, r = roll * d;
    const float sp = std::sin(p), cp = std::cos(p);
    const float sy = std::sin(y), cy = std::cos(y);
    const float sr = std::sin(r), cr = std::cos(r);
    return {
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    };
}

Quat quatMul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Vec3 rightFromQuat(const Quat& q) {
    return {
        1.f - 2.f * (q.y * q.y + q.z * q.z),
        2.f * (q.x * q.y + q.w * q.z),
        2.f * (q.x * q.z - q.w * q.y),
    };
}

bool looksLikeWorldPos(const float* p) {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    // Reject near-zero / quat-like / huge values
    const float ax = std::fabs(p[0]), ay = std::fabs(p[1]), az = std::fabs(p[2]);
    if (ax < 0.01f && ay < 0.01f && az < 0.01f) return false;
    if (ax > 3e6f || ay > 3e6f || az > 3e6f) return false;
    // Y usually in reasonable range for player eye
    if (ay > 5000.f) return false;
    return true;
}

// Candidate offsets for Vec3 position on camera / blend objects (relative to pointer)
constexpr std::uintptr_t kPosCandidates[] = {
    0x00, 0x08, 0x10, 0x18, 0x1C, 0x20, 0x30, 0x38, 0x40, 0x48, 0x50, 0x60,
};

void tryApplyLateral(void* obj, const Quat& orient, float lateral) {
    if (!obj || std::fabs(lateral) < 0.0005f) return;
    auto* base = reinterpret_cast<char*>(obj);
    Vec3 right = rightFromQuat(orient);
    right.y = 0.f;
    const float rl = std::sqrt(right.x * right.x + right.z * right.z);
    if (rl < 1e-4f) return;
    right.x /= rl;
    right.z /= rl;

    for (const auto off : kPosCandidates) {
        // Skip overlap with known quat region when obj is the camera component
        auto* p = reinterpret_cast<float*>(base + off);
        if (!looksLikeWorldPos(p)) continue;
        p[0] += right.x * lateral;
        p[2] += right.z * lateral;
        // Apply to first plausible slot only (avoid double-shift)
        break;
    }
}

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blendState, float factor) {
    if (g_orig) g_orig(component, blendState, factor);
    // Pass both pointers so lateral can try component + blendState
    auto& mod = taczlean::LeanModule::get();
    mod.onCameraBlend(component, 1.f / 60.f);
    // Second pass for blendState position if present
    if (blendState) {
        mod.applyLateralOnly(blendState);
    }
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
    float target = 0.f;
    float lat = 0.f;
    if (m_left.load() && !m_right.load()) {
        target = -m_maxLeanDeg;
        lat = -m_lateralOffset;
    } else if (m_right.load() && !m_left.load()) {
        target = m_maxLeanDeg;
        lat = m_lateralOffset;
    }
    m_targetAngle = target;
    m_targetLateral = lat;
}

void LeanModule::applyLateralOnly(void* anyCameraObj) {
    if (!m_enabled || !m_enableLateral || !anyCameraObj) return;
    if (std::fabs(m_currentLateral) < 0.0005f) return;
    auto* base = reinterpret_cast<char*>(anyCameraObj);
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    if (qlen > 0.5f && qlen < 1.5f) {
        tryApplyLateral(anyCameraObj, cur, m_currentLateral);
    } else {
        // No quat at 0x28 — still try position candidates with last known identity right = +X
        Quat id{0, 0, 0, 1};
        tryApplyLateral(anyCameraObj, id, m_currentLateral);
    }
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

    // Lateral translation (peek past cover)
    if (m_enableLateral && std::fabs(m_currentLateral) > 0.0005f) {
        tryApplyLateral(cameraComponent, cur, m_currentLateral);
    }

    // Rotation: roll + slight yaw in lean direction (helps peek even if pos fails)
    float pitch = 0.f;
    float yaw = 0.f;
    float roll = 0.f;
    if (m_enableRoll && std::fabs(m_currentAngle) > 0.01f) {
        roll = m_currentAngle * m_firstPersonRollMult;
    }
    // Yaw peek proportional to lateral strength (degrees): lateral 0.22 ~ few degrees
    if (m_enableLateral && std::fabs(m_currentLateral) > 0.0005f) {
        // Map blocks offset to ~yaw degrees: 0.22 blocks -> ~8 deg at default
        yaw = (m_currentLateral / 0.22f) * 8.f;
    }

    if (std::fabs(pitch) > 0.001f || std::fabs(yaw) > 0.001f || std::fabs(roll) > 0.001f) {
        Quat bias = quatFromEulerDeg(pitch, yaw, roll);
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
