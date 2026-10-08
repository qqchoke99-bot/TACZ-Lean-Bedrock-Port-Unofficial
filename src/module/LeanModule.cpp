#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr std::uintptr_t kRotationOffset = 0x28;

struct Quat { float x, y, z, w; };

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

using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_orig = nullptr;

void cameraBlendHook(void* component, void* blend, float factor) {
    if (g_orig) g_orig(component, blend, factor);
    // dt approx 1/60; LeanModule tracks time internally via smooth factor per call
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
    m_targetAngle = m_currentAngle = m_prevAngle = 0.f;
}

void LeanModule::setLeanLeft(bool held) { m_left = held; }
void LeanModule::setLeanRight(bool held) { m_right = held; }

void LeanModule::onCameraBlend(void* cameraComponent, float /*dt*/) {
    if (!m_enabled || !cameraComponent) return;

    // Target like Java ClientEvents.onClientTick
    float target = 0.f;
    if (m_left.load() && !m_right.load()) target = -m_maxLeanDeg;
    else if (m_right.load() && !m_left.load()) target = m_maxLeanDeg;
    m_targetAngle = target;

    m_prevAngle = m_currentAngle;
    // Smooth toward target (SMOOTH_FACTOR 0.25 per tick ≈ strong lerp)
    m_currentAngle += (m_targetAngle - m_currentAngle) * m_smoothFactor;

    if (std::fabs(m_currentAngle) < 0.01f && std::fabs(m_targetAngle) < 0.01f) {
        m_currentAngle = 0.f;
        return;
    }

    // Partial-tick style: use current (camera runs every frame)
    const float angle = m_currentAngle;
    const float rollDeg = angle * m_firstPersonRollMult;

    auto* q = reinterpret_cast<float*>(reinterpret_cast<char*>(cameraComponent) + kRotationOffset);
    Quat cur{q[0], q[1], q[2], q[3]};
    const float len = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    if (!(len > 0.5f && len < 1.5f)) return;

    Quat bias = quatFromRollDeg(rollDeg);
    Quat out = quatMul(cur, bias);
    const float n = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (n > 1e-6f) {
        q[0] = out.x / n;
        q[1] = out.y / n;
        q[2] = out.z / n;
        q[3] = out.w / n;
    }
}

void LeanModule::loadConfig(const nlohmann::json& j) {
    auto gf = [&](const char* k, float& v) {
        if (j.contains(k) && j[k].is_number()) v = j[k].get<float>();
    };
    auto gi = [&](const char* k, int& v) {
        if (j.contains(k) && j[k].is_number_integer()) v = j[k].get<int>();
        else if (j.contains(k) && j[k].is_number()) v = static_cast<int>(j[k].get<float>());
    };
    auto gb = [&](const char* k, bool& v) {
        if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>();
    };
    gb("enabled", m_enabled);
    gf("maxLeanDeg", m_maxLeanDeg);
    gf("smoothFactor", m_smoothFactor);
    gf("firstPersonRollMult", m_firstPersonRollMult);
    gb("showButtons", m_showButtons);
    gi("buttonBgSize", m_buttonBgSize);
    gi("buttonIconSize", m_buttonIconSize);
    gi("buttonLeftX", m_buttonLeftX);
    gi("buttonLeftY", m_buttonLeftY);
    gi("buttonRightX", m_buttonRightX);
    gi("buttonRightY", m_buttonRightY);
    gf("buttonIconOpacity", m_buttonIconOpacity);
    gf("buttonBgOpacity", m_buttonBgOpacity);
}

void LeanModule::saveConfig(nlohmann::json& j) const {
    j["enabled"] = m_enabled;
    j["maxLeanDeg"] = m_maxLeanDeg;
    j["smoothFactor"] = m_smoothFactor;
    j["firstPersonRollMult"] = m_firstPersonRollMult;
    j["showButtons"] = m_showButtons;
    j["buttonBgSize"] = m_buttonBgSize;
    j["buttonIconSize"] = m_buttonIconSize;
    j["buttonLeftX"] = m_buttonLeftX;
    j["buttonLeftY"] = m_buttonLeftY;
    j["buttonRightX"] = m_buttonRightX;
    j["buttonRightY"] = m_buttonRightY;
    j["buttonIconOpacity"] = m_buttonIconOpacity;
    j["buttonBgOpacity"] = m_buttonBgOpacity;
}

} // namespace taczlean
