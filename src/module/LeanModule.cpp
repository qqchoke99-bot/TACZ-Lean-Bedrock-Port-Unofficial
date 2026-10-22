#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TaczLean", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "TaczLean", __VA_ARGS__)

namespace {

// Camera component layout (1.26.x CameraBlendSystem):
constexpr std::uintptr_t kRotationOffset = 0x28; // quat xyzw
constexpr std::uintptr_t kPosXOffset     = 0x38;
constexpr std::uintptr_t kPosYOffset     = 0x3C;
constexpr std::uintptr_t kPosZOffset     = 0x40;

// Model part local quat candidate (probe: often identity at +0x20c)
constexpr std::uintptr_t kPartQuatOffset = 0x20c;

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

// Camera right vector on XZ (for lateral peek without yaw)
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
    if (std::fabs(x) < 1e-4f && std::fabs(y) < 1e-4f && std::fabs(z) < 1e-4f) return false;
    if (std::fabs(x) > 3e6f || std::fabs(z) > 3e6f) return false;
    if (y < -128.f || y > 4000.f) return false;
    return true;
}

bool looksLikeQuat(const Quat& q) {
    if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z) || !std::isfinite(q.w))
        return false;
    const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    return n2 > 0.5f && n2 < 1.5f;
}

// ---- CameraBlend hook: orig (blend + other mods / shake) THEN lean ----
using CameraBlendFn = void (*)(void*, void*, float);
CameraBlendFn g_origBlend = nullptr;

void cameraBlendHook(void* component, void* blendState, float factor) {
    if (g_origBlend) g_origBlend(component, blendState, factor);
    // Lean applied AFTER original chain so we compose on top of shake/CO.
    taczlean::LeanModule::get().onCameraBlend(component, 1.f / 60.f);
}

using NormalTickFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
NormalTickFn g_origNormalTick = nullptr;
std::uint64_t s_bodyTickCount = 0;

void normalTickHook(void* a0, void* a1, void* a2, void* a3,
                    void* a4, void* a5, void* a6, void* a7) {
    if (g_origNormalTick)
        g_origNormalTick(a0, a1, a2, a3, a4, a5, a6, a7);
    // Second chance: re-apply lean if a late system overwrote the camera.
    taczlean::LeanModule::get().onBodyTick();
}

struct TrackedPart {
    void* addr = nullptr;
    char name[16]{};
    enum class Kind { Head, Body, Arm, Other } kind = Kind::Other;
};
std::vector<TrackedPart> g_trackedParts;

TrackedPart::Kind classifyPart(const char* name, unsigned long long len) {
    auto eq = [&](const char* s) {
        return std::strlen(s) == len && std::memcmp(name, s, len) == 0;
    };
    if (eq("head") || eq("hat")) return TrackedPart::Kind::Head;
    if (eq("body") || eq("waist")) return TrackedPart::Kind::Body;
    if (eq("leftArm") || eq("rightArm") || eq("left_arm") || eq("right_arm"))
        return TrackedPart::Kind::Arm;
    return TrackedPart::Kind::Other;
}

} // namespace

namespace taczlean {

LeanModule& LeanModule::get() {
    static LeanModule inst;
    return inst;
}

void LeanModule::init() {
    if (!m_hooked) {
        const auto addr = cameraoverhaul::memory::resolve(
            cameraoverhaul::memory::SignatureId::CameraBlendSystemTick);
        if (addr &&
            cameraoverhaul::hooks::install(reinterpret_cast<void*>(addr),
                                           reinterpret_cast<void*>(cameraBlendHook),
                                           reinterpret_cast<void**>(&g_origBlend))) {
            m_hooked = true;
            LOGI("CameraBlend hooked @ 0x%lx (lean after blend/shake)",
                 static_cast<unsigned long>(addr));
        } else {
            LOGW("CameraBlend hook FAILED — lean camera disabled");
        }
    }

    if (!m_tickHooked) {
        const auto tickAddr = cameraoverhaul::memory::resolve(
            cameraoverhaul::memory::SignatureId::NormalTick);
        if (tickAddr &&
            cameraoverhaul::hooks::install(reinterpret_cast<void*>(tickAddr),
                                           reinterpret_cast<void*>(normalTickHook),
                                           reinterpret_cast<void**>(&g_origNormalTick))) {
            m_tickHooked = true;
            LOGI("NormalTick hooked @ 0x%lx (shake-compat re-apply)",
                 static_cast<unsigned long>(tickAddr));
        } else {
            LOGW("NormalTick not hooked — shake-compat limited");
        }
    }

    // RegisterModelPart optional — omitted so build works when SignatureId
    // only has NormalTick + CameraBlendSystemTick (older headers).

    LOGI("TaczLean init enabled=%d roll=%d lateral=%d shakeCompat=%d body=%d",
         m_enabled ? 1 : 0, m_enableRoll ? 1 : 0, m_enableLateral ? 1 : 0,
         m_shakeCompat ? 1 : 0, m_bodyLeanEnabled ? 1 : 0);
}

void LeanModule::shutdown() {
    m_left = false;
    m_right = false;
    m_toggleLeft = m_toggleRight = false;
    m_targetAngle = m_currentAngle = 0.f;
    m_targetLateral = m_currentLateral = 0.f;
    m_lastCamera = nullptr;
    g_trackedParts.clear();
    s_bodyTickCount = 0;
}

void LeanModule::onRegisterPart(void* owner, void* /*pair*/, const char* name,
                                unsigned long long len, void* /*result*/) {
    if (!name || len == 0 || len > 15 || !owner) return;
    const auto kind = classifyPart(name, len);
    if (kind == TrackedPart::Kind::Other) return;

    for (const auto& tp : g_trackedParts)
        if (tp.addr == owner) return;

    TrackedPart tp;
    tp.addr = owner;
    tp.kind = kind;
    const size_t n = static_cast<size_t>(len);
    std::memcpy(tp.name, name, n);
    tp.name[n] = '\0';
    g_trackedParts.push_back(tp);

    if (m_debugProbe)
        LOGI("track part '%s' kind=%d @ %p (n=%zu)", tp.name, static_cast<int>(kind),
             owner, g_trackedParts.size());
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
        // Positive angle = lean left on this Bedrock camera path
        ang = m_maxLeanDeg;
        lat = -m_lateralOffset; // shift eye left
    } else if (m_right.load() && !m_left.load()) {
        ang = -m_maxLeanDeg;
        lat = m_lateralOffset;
    }
    m_targetAngle = ang;
    m_targetLateral = lat;
}

void LeanModule::smoothStep() {
    const float s = std::clamp(m_smoothFactor, 0.05f, 1.f);
    m_currentAngle += (m_targetAngle - m_currentAngle) * s;
    m_currentLateral += (m_targetLateral - m_currentLateral) * s;
    if (std::fabs(m_currentAngle) < 0.01f && std::fabs(m_targetAngle) < 0.01f)
        m_currentAngle = 0.f;
    if (std::fabs(m_currentLateral) < 1e-5f && std::fabs(m_targetLateral) < 1e-5f)
        m_currentLateral = 0.f;
}

void LeanModule::applyToCamera(void* cameraComponent) {
    if (!cameraComponent) return;
    if (!m_enableRoll && !m_enableLateral) return;
    if (std::fabs(m_currentAngle) < 0.01f && std::fabs(m_currentLateral) < 1e-5f) return;

    auto* base = reinterpret_cast<std::uint8_t*>(cameraComponent);
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    if (!looksLikeQuat(cur)) return;
    cur = quatNormalize(cur);

    // --- Lateral peek (XZ only) — composed ON TOP of current pos (keeps shake) ---
    if (m_enableLateral && std::fabs(m_currentLateral) > 1e-5f) {
        auto* px = reinterpret_cast<float*>(base + kPosXOffset);
        auto* py = reinterpret_cast<float*>(base + kPosYOffset);
        auto* pz = reinterpret_cast<float*>(base + kPosZOffset);
        if (looksLikePos(*px, *py, *pz)) {
            const Vec3 right = rightFromQuat(cur);
            *px += right.x * m_currentLateral;
            *pz += right.z * m_currentLateral;
        }
    }

    // --- Roll — multiply onto current quat so CO/shake rotation is preserved ---
    if (m_enableRoll && std::fabs(m_currentAngle) > 0.01f) {
        const float h = m_currentAngle * m_firstPersonRollMult * 0.01745329251f * 0.5f;
        const Quat rollQ{0.f, 0.f, std::sin(h), std::cos(h)};
        const Quat out = quatNormalize(quatMul(cur, rollQ));
        qf[0] = out.x;
        qf[1] = out.y;
        qf[2] = out.z;
        qf[3] = out.w;
    }
}

void LeanModule::applyBodyParts() {
    if (!m_bodyLeanEnabled || g_trackedParts.empty()) return;
    if (std::fabs(m_currentAngle) < 0.05f) return;

    for (const auto& tp : g_trackedParts) {
        if (!tp.addr) continue;
        float scale = 1.f;
        switch (tp.kind) {
        case TrackedPart::Kind::Head: scale = m_headScale; break;
        case TrackedPart::Kind::Body: scale = m_bodyScale; break;
        case TrackedPart::Kind::Arm:  scale = m_armScale; break;
        default: continue;
        }
        if (std::fabs(scale) < 1e-4f) continue;

        const float deg = m_currentAngle * scale;
        const float h = deg * 0.01745329251f * 0.5f;
        const Quat leanQ{0.f, 0.f, std::sin(h), std::cos(h)};

        auto* qf = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(tp.addr) +
                                            kPartQuatOffset);
        Quat cur{qf[0], qf[1], qf[2], qf[3]};
        if (!looksLikeQuat(cur)) continue;
        // Compose lean onto whatever animation wrote this frame
        const Quat out = quatNormalize(quatMul(quatNormalize(cur), leanQ));
        qf[0] = out.x;
        qf[1] = out.y;
        qf[2] = out.z;
        qf[3] = out.w;
    }
}

void LeanModule::onCameraBlend(void* cameraComponent, float /*dt*/) {
    if (!m_enabled || !cameraComponent) return;

    m_lastCamera = cameraComponent;
    updateTargets();
    smoothStep();
    applyToCamera(cameraComponent);
}

void LeanModule::onBodyTick() {
    ++s_bodyTickCount;
    if (s_bodyTickCount == 1 || (s_bodyTickCount % 3600) == 0)
        LOGI("tick alive n=%llu cam=%p parts=%zu shakeCompat=%d",
             static_cast<unsigned long long>(s_bodyTickCount), m_lastCamera,
             g_trackedParts.size(), m_shakeCompat ? 1 : 0);

    if (!m_enabled) return;

    // Keep smoothing alive even if blend is skipped some frames
    updateTargets();
    smoothStep();

    // Re-apply lean after full tick so late camera shake cannot wipe it
    if (m_shakeCompat && m_lastCamera)
        applyToCamera(m_lastCamera);

    applyBodyParts();
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
    gb("bodyLeanEnabled", m_bodyLeanEnabled);
    gf("headScale", m_headScale);
    gf("bodyScale", m_bodyScale);
    gf("armScale", m_armScale);
    gb("shakeCompat", m_shakeCompat);
    gb("debugProbe", m_debugProbe);
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
    j["bodyLeanEnabled"] = m_bodyLeanEnabled;
    j["headScale"] = m_headScale;
    j["bodyScale"] = m_bodyScale;
    j["armScale"] = m_armScale;
    j["shakeCompat"] = m_shakeCompat;
    j["debugProbe"] = m_debugProbe;
}

} // namespace taczlean
