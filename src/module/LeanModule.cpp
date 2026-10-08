#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// From CameraBlendSystem disasm (1.26.x):
//   ldur q0, [x0, #0x28]  -> rotation quat (16 bytes)
//   ldr  s0, [x0, #0x38]  -> float
//   ldr  s0, [x0, #0x3c]  -> float
//   ldr  s0, [x0, #0x40]  -> float
// Confirmed: CO uses +0x28 for rotation. Triple at +0x38 is position candidate.
constexpr std::uintptr_t kRotationOffset = 0x28;
constexpr std::uintptr_t kPosXOffset     = 0x38;
constexpr std::uintptr_t kPosYOffset     = 0x3C;
constexpr std::uintptr_t kPosZOffset     = 0x40;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };

Quat quatNormalize(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-8f)) return {0.f, 0.f, 0.f, 1.f};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
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

bool looksLikePos(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    // Reject near-zero (unset) and absurd world coords
    if (std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f && std::fabs(z) < 1e-4f) return false;
    if (std::fabs(x) > 3e6f || std::fabs(z) > 3e6f) return false;
    if (y < -128.f || y > 4000.f) return false;
    return true;
}

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blendState, float factor) {
    if (g_orig) g_orig(component, blendState, factor);
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
    float ang = 0.f;
    float lat = 0.f;
    if (m_left.load() && !m_right.load()) {
        ang = -m_maxLeanDeg;
        lat = -m_lateralOffset;
    } else if (m_right.load() && !m_left.load()) {
        ang = m_maxLeanDeg;
        lat = m_lateralOffset;
    }
    m_targetAngle = ang;
    m_targetLateral = lat;
}

void LeanModule::applyLateralOnly(void*) {}

void LeanModule::onCameraBlend(void* cameraComponent, float /*dt*/) {
    if (!m_enabled || !cameraComponent) return;

    updateTargets();

    const float s = std::clamp(m_smoothFactor, 0.05f, 1.f);
    m_currentAngle += (m_targetAngle - m_currentAngle) * s;
    m_currentLateral += (m_targetLateral - m_currentLateral) * s;
    if (std::fabs(m_currentAngle) < 0.01f && std::fabs(m_targetAngle) < 0.01f)
        m_currentAngle = 0.f;
    if (std::fabs(m_currentLateral) < 1e-5f && std::fabs(m_targetLateral) < 1e-5f)
        m_currentLateral = 0.f;

    auto* base = reinterpret_cast<char*>(cameraComponent);

    // --- Rotation quat @ +0x28 (proven stable) ---
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    if (!(qlen > 0.5f && qlen < 1.5f)) return;
    cur = quatNormalize(cur);

    // --- Position @ +0x38,+0x3c,+0x40 (from CameraBlendSystem copy) ---
    if (m_enableLateral && std::fabs(m_currentLateral) > 1e-5f) {
        auto* px = reinterpret_cast<float*>(base + kPosXOffset);
        auto* py = reinterpret_cast<float*>(base + kPosYOffset);
        auto* pz = reinterpret_cast<float*>(base + kPosZOffset);
        if (looksLikePos(*px, *py, *pz)) {
            const Vec3 right = rightFromQuat(cur);
            // Only shift XZ — keep look direction, no pitch/yaw
            *px += right.x * m_currentLateral;
            *pz += right.z * m_currentLateral;
        }
    }

    // --- Optional roll only (no yaw / pitch) ---
    if (m_enableRoll && std::fabs(m_currentAngle) > 0.01f) {
        const float h = m_currentAngle * m_firstPersonRollMult * 0.01745329251f * 0.5f;
        const Quat rollQ{0.f, 0.f, std::sin(h), std::cos(h)};
        Quat out = quatNormalize(quatMul(cur, rollQ));
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
