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

Quat quatNormalize(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-8f)) return {0.f, 0.f, 0.f, 1.f};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// Horizontal right from camera quat (XZ only)
Vec3 rightFromQuat(const Quat& q) {
    Vec3 r{
        1.f - 2.f * (q.y * q.y + q.z * q.z),
        0.f,
        2.f * (q.x * q.z - q.w * q.y),
    };
    const float len = std::sqrt(r.x * r.x + r.z * r.z);
    if (len > 1e-5f) {
        r.x /= len;
        r.z /= len;
    } else {
        r = {1.f, 0.f, 0.f};
    }
    return r;
}

/*
 * Pure eye-position shift. No yaw / pitch.
 * Write several fixed slots relative to camera component layout:
 *   - Vec3 candidates near known quat @ 0x28
 *   - mat4 translation (column-major m[12], m[14])
 */
void applyPositionShift(void* obj, const Vec3& right, float lateral) {
    if (!obj || std::fabs(lateral) < 1e-5f) return;
    auto* base = reinterpret_cast<char*>(obj);

    const std::uintptr_t vecOffs[] = {
        0x00, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C, 0x20,
        0x34, 0x38, 0x3C, 0x40, 0x48, 0x50, 0x58, 0x60,
        0x68, 0x70, 0x80, 0x90, 0xA0, 0xB0,
    };

    for (auto off : vecOffs) {
        auto* p = reinterpret_cast<float*>(base + off);
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
        // Always apply XZ shift (aggressive — needed when heuristics skipped the real field)
        p[0] += right.x * lateral;
        p[2] += right.z * lateral;
    }

    // mat4 @ 0 translation
    auto* m = reinterpret_cast<float*>(base);
    if (std::isfinite(m[12]) && std::isfinite(m[14])) {
        m[12] += right.x * lateral;
        m[14] += right.z * lateral;
    }
}

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blendState, float factor) {
    // Run game first so component holds the final camera for this frame
    if (g_orig) g_orig(component, blendState, factor);
    auto& mod = taczlean::LeanModule::get();
    mod.onCameraBlend(component, 1.f / 60.f);
    if (blendState) mod.applyLateralOnly(blendState);
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
    float lat = 0.f;
    float ang = 0.f;
    if (m_left.load() && !m_right.load()) {
        lat = -m_lateralOffset;
        ang = -m_maxLeanDeg;
    } else if (m_right.load() && !m_left.load()) {
        lat = m_lateralOffset;
        ang = m_maxLeanDeg;
    }
    m_targetLateral = lat;
    m_targetAngle = ang;
}

void LeanModule::applyLateralOnly(void* anyCameraObj) {
    if (!m_enabled || !m_enableLateral || !anyCameraObj) return;
    if (std::fabs(m_currentLateral) < 1e-5f) return;

    Vec3 right{1.f, 0.f, 0.f};
    auto* qf = reinterpret_cast<float*>(reinterpret_cast<char*>(anyCameraObj) + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    if (qlen > 0.5f && qlen < 1.5f) right = rightFromQuat(quatNormalize(cur));

    applyPositionShift(anyCameraObj, right, m_currentLateral);
}

void LeanModule::onCameraBlend(void* cameraComponent, float /*dt*/) {
    if (!m_enabled || !cameraComponent) return;

    updateTargets();

    const float s = std::clamp(m_smoothFactor, 0.05f, 1.f);
    m_currentLateral += (m_targetLateral - m_currentLateral) * s;
    m_currentAngle += (m_targetAngle - m_currentAngle) * s;
    if (std::fabs(m_currentLateral) < 1e-5f && std::fabs(m_targetLateral) < 1e-5f)
        m_currentLateral = 0.f;

    auto* base = reinterpret_cast<char*>(cameraComponent);
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);

    Vec3 right{1.f, 0.f, 0.f};
    if (qlen > 0.5f && qlen < 1.5f) {
        cur = quatNormalize(cur);
        right = rightFromQuat(cur);
    }

    // 1) Position only — same look direction, eye moves sideways
    if (m_enableLateral && std::fabs(m_currentLateral) > 1e-5f) {
        applyPositionShift(cameraComponent, right, m_currentLateral);
    }

    // 2) Optional roll (tilt) — no yaw, no pitch
    if (m_enableRoll && qlen > 0.5f && std::fabs(m_currentAngle) > 0.01f) {
        const float h = m_currentAngle * m_firstPersonRollMult * 0.01745329251f * 0.5f;
        const float sz = std::sin(h), cz = std::cos(h);
        Quat out{
            cur.w * 0.f + cur.x * cz + cur.y * sz - cur.z * 0.f,
            cur.w * 0.f - cur.x * sz + cur.y * cz + cur.z * 0.f,
            cur.w * sz + cur.x * 0.f - cur.y * 0.f + cur.z * cz,
            cur.w * cz - cur.x * 0.f - cur.y * 0.f - cur.z * sz,
        };
        // cur * roll(z)
        out = {
            cur.x * cz + cur.y * sz,
            -cur.x * sz + cur.y * cz,
            cur.z * cz + cur.w * sz,
            -cur.z * sz + cur.w * cz,
        };
        // Correct quat multiply cur * (0,0,sin,cos)
        const Quat r{0.f, 0.f, sz, cz};
        out = {
            cur.w * r.x + cur.x * r.w + cur.y * r.z - cur.z * r.y,
            cur.w * r.y - cur.x * r.z + cur.y * r.w + cur.z * r.x,
            cur.w * r.z + cur.x * r.y - cur.y * r.x + cur.z * r.w,
            cur.w * r.w - cur.x * r.x - cur.y * r.y - cur.z * r.z,
        };
        out = quatNormalize(out);
        qf[0] = out.x;
        qf[1] = out.y;
        qf[2] = out.z;
        qf[3] = out.w;
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
