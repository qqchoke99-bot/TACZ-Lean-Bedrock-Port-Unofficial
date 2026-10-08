#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uintptr_t kRotationOffset = 0x28;
constexpr float kDeg2Rad = 0.01745329251f;

struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };

Quat quatMul(const Quat& a, const Quat& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat quatNormalize(const Quat& q) {
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-8f)) return {0.f, 0.f, 0.f, 1.f};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// Axis-angle (axis must be unit)
Quat axisAngle(float ax, float ay, float az, float angleRad) {
    const float h = angleRad * 0.5f;
    const float s = std::sin(h);
    return {ax * s, ay * s, az * s, std::cos(h)};
}

// Camera local right (horizontal) from quat
Vec3 rightFromQuat(const Quat& q) {
    Vec3 r{
        1.f - 2.f * (q.y * q.y + q.z * q.z),
        2.f * (q.x * q.y + q.w * q.z),
        2.f * (q.x * q.z - q.w * q.y),
    };
    r.y = 0.f;
    const float len = std::sqrt(r.x * r.x + r.z * r.z);
    if (len > 1e-5f) {
        r.x /= len;
        r.z /= len;
    }
    return r;
}

bool looksLikeWorldPos(const float* p) {
    if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
    const float ax = std::fabs(p[0]), ay = std::fabs(p[1]), az = std::fabs(p[2]);
    if (ax < 0.01f && ay < 0.01f && az < 0.01f) return false;
    if (ax > 3e6f || az > 3e6f || ay > 5000.f) return false;
    return true;
}

constexpr std::uintptr_t kPosCandidates[] = {
    0x00, 0x08, 0x10, 0x18, 0x1C, 0x20, 0x30, 0x38, 0x40, 0x48, 0x50, 0x60,
};

void tryShiftPosition(void* obj, const Vec3& right, float lateral) {
    if (!obj || std::fabs(lateral) < 0.0005f) return;
    auto* base = reinterpret_cast<char*>(obj);
    for (const auto off : kPosCandidates) {
        auto* p = reinterpret_cast<float*>(base + off);
        if (!looksLikeWorldPos(p)) continue;
        p[0] += right.x * lateral;
        p[2] += right.z * lateral;
        break;
    }
}

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blendState, float factor) {
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
    // Left lean: negative angle, shift camera to the left (-right)
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
    Vec3 right = (qlen > 0.5f && qlen < 1.5f) ? rightFromQuat(cur) : Vec3{1.f, 0.f, 0.f};
    tryShiftPosition(anyCameraObj, right, m_currentLateral);
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
    cur = quatNormalize(cur);

    const Vec3 right = rightFromQuat(cur);

    // 1) Shift eye left/right (true peek past wall)
    if (m_enableLateral && std::fabs(m_currentLateral) > 0.0005f) {
        tryShiftPosition(cameraComponent, right, m_currentLateral);
    }

    // 2) Rotation — ONLY roll (tilt) + horizontal yaw around world UP
    //    Never pitch (no look up/down)
    Quat out = cur;

    if (m_enableRoll && std::fabs(m_currentAngle) > 0.01f) {
        const float rollRad = m_currentAngle * m_firstPersonRollMult * kDeg2Rad;
        // Roll around view-forward-ish: use local Z like CameraOverhaul
        out = quatMul(out, axisAngle(0.f, 0.f, 1.f, rollRad));
    }

    // Horizontal peek yaw around world Y only (helps see past corner)
    // Scale with lateral slider: at 0.22 blocks → ~10° yaw toward lean side
    if (m_enableLateral && std::fabs(m_currentLateral) > 0.0005f) {
        const float yawDeg = (m_currentLateral / 0.22f) * 10.f;
        const float yawRad = yawDeg * kDeg2Rad;
        // World-up yaw applied on the left so it stays horizon-level
        out = quatMul(axisAngle(0.f, 1.f, 0.f, yawRad), out);
    }

    out = quatNormalize(out);
    qf[0] = out.x;
    qf[1] = out.y;
    qf[2] = out.z;
    qf[3] = out.w;
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
