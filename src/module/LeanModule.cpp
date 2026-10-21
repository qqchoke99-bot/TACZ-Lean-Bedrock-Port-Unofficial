#include "module/LeanModule.hpp"
#include "core/memory/Hooks.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TaczLean", __VA_ARGS__)

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

// Forwarding-only hook for the (previously unused) NormalTick signature.
// Declared with the full 8 register-argument slots so the original call's
// arguments are never clobbered; it must not disturb the game call.
using NormalTickFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);
NormalTickFn g_origNormalTick = nullptr;
std::uint64_t s_bodyTickCount = 0;

void normalTickHook(void* a0, void* a1, void* a2, void* a3,
                    void* a4, void* a5, void* a6, void* a7) {
    if (g_origNormalTick)
        g_origNormalTick(a0, a1, a2, a3, a4, a5, a6, a7);
    taczlean::LeanModule::get().onBodyTick();
}

// Probe hook: data-driven model part registration (owner, pair, name, len, arg).
using RegisterPartFn = void* (*)(void*, void*, const char*, unsigned long long, void*);
RegisterPartFn g_origRegisterPart = nullptr;

void* registerPartHook(void* owner, void* pair, const char* name,
                       unsigned long long len, void* arg) {
    void* r = g_origRegisterPart(owner, pair, name, len, arg);
    taczlean::LeanModule::get().onRegisterPart(owner, pair, name, len, r);
    return r;
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
    if (addr &&
        cameraoverhaul::hooks::install(reinterpret_cast<void*>(addr),
                                       reinterpret_cast<void*>(cameraBlendHook),
                                       reinterpret_cast<void**>(&g_orig))) {
        m_hooked = true;
    }

    // Body tick source: independent of the camera hook so body-part lean can
    // run (and be verified via logcat) even if the camera signature drifts.
    if (!m_tickHooked) {
        const auto tickAddr = cameraoverhaul::memory::resolve(
            cameraoverhaul::memory::SignatureId::NormalTick);
        if (tickAddr) {
            m_tickHooked = cameraoverhaul::hooks::install(
                reinterpret_cast<void*>(tickAddr),
                reinterpret_cast<void*>(normalTickHook),
                reinterpret_cast<void**>(&g_origNormalTick)) != nullptr;
            if (m_tickHooked)
                LOGI("NormalTick hooked @ 0x%lx", static_cast<unsigned long>(tickAddr));
            else
                LOGI("NormalTick hook install failed");
        } else {
            LOGI("NormalTick signature not resolved");
        }
    }

    // Probe: part-name registration — tells us at runtime which model objects
    // own the humanoid bones (head/body/arms) and their live addresses.
    if (!m_partHooked) {
        const auto partAddr = cameraoverhaul::memory::resolve(
            cameraoverhaul::memory::SignatureId::RegisterModelPart);
        if (partAddr) {
            m_partHooked = cameraoverhaul::hooks::install(
                reinterpret_cast<void*>(partAddr),
                reinterpret_cast<void*>(registerPartHook),
                reinterpret_cast<void**>(&g_origRegisterPart)) != nullptr;
            if (m_partHooked)
                LOGI("RegisterModelPart hooked @ 0x%lx", static_cast<unsigned long>(partAddr));
            else
                LOGI("RegisterModelPart hook install failed");
        } else {
            LOGI("RegisterModelPart signature not resolved");
        }
    }
}

void LeanModule::shutdown() {
    m_left = false;
    m_right = false;
    m_toggleLeft = m_toggleRight = false;
    m_targetAngle = m_currentAngle = 0.f;
    m_targetLateral = m_currentLateral = 0.f;
    m_bodyLogRequested = false;
    m_bodyLogCooldown = 0;
    s_bodyTickCount = 0;
}

void LeanModule::onRegisterPart(void* owner, void* pair, const char* name,
                                unsigned long long len, void* result) {
    if (!name || len == 0 || len > 32) return;
    static const char* const kInteresting[] = {
        "head", "body", "rightArm", "leftArm", "rightLeg", "leftLeg",
        "left_arm", "right_arm", "left_leg", "right_leg", "hat", "waist",
    };
    bool match = false;
    for (const char* k : kInteresting) {
        if (std::strlen(name) == len && std::memcmp(name, k, len) == 0) {
            match = true;
            break;
        }
    }
    if (!match) return;

    static std::uint64_t s_partCount = 0;
    ++s_partCount;
    // Log the first 40 matches in full; afterwards only every 200th.
    if (s_partCount > 40 && (s_partCount % 200) != 0) return;

    // owner object hex dump (first 0x50 bytes) — lets us locate rot/pos fields
    char hex[0x50 * 2 + 1];
    const auto* p = static_cast<const unsigned char*>(owner);
    for (std::size_t i = 0; i < 0x50; ++i)
        std::snprintf(&hex[i * 2], 3, "%02x", p[i]);
    hex[0x50 * 2] = '\0';

    void* pair2 = pair ? *reinterpret_cast<void* const*>(static_cast<const char*>(pair) + 8) : nullptr;
    void* factory = pair2 ? *reinterpret_cast<void* const*>(static_cast<const char*>(pair2) + 0x28) : nullptr;

    LOGI("part[%llu] '%.*s' owner=0x%lx pair2=0x%lx factory=0x%lx ret=0x%lx dump=%s",
         static_cast<unsigned long long>(s_partCount), static_cast<int>(len), name,
         reinterpret_cast<unsigned long>(owner),
         reinterpret_cast<unsigned long>(pair2),
         reinterpret_cast<unsigned long>(factory),
         reinterpret_cast<unsigned long>(result), hex);
}

void LeanModule::onBodyTick() {
    ++s_bodyTickCount;
    // First call confirms the hook fires; afterwards keep it quiet (1 line
    // per ~2400 calls) so logcat stays readable.
    if (s_bodyTickCount == 1 || (s_bodyTickCount % 2400) == 0)
        LOGI("body tick alive, calls=%llu",
             static_cast<unsigned long long>(s_bodyTickCount));
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
        // Native camera API renders roll with the opposite sign vs. the
        // original Java mod, so "lean left" must emit a POSITIVE angle
        // (negative rendered as a right roll in-game).
        ang = m_maxLeanDeg;
        lat = -m_lateralOffset;
    } else if (m_right.load() && !m_left.load()) {
        ang = -m_maxLeanDeg;
        lat = m_lateralOffset;
    }
    if (ang != m_targetAngle || lat != m_targetLateral)
        m_bodyLogRequested = true;
    m_targetAngle = ang;
    m_targetLateral = lat;
}

void LeanModule::applyLateralOnly(void*) {}

void LeanModule::logBodyState() {
    if (m_bodyLogRequested) {
        m_bodyLogRequested = false;
        m_bodyLogCooldown = 30;
        LOGI("body lean target: head %.1f / torso %.1f / arms %.1f deg, lateral %.3f",
             static_cast<double>(headLeanDeg()), static_cast<double>(bodyLeanDeg()),
             static_cast<double>(armLeanDeg()), static_cast<double>(m_targetLateral));
        return;
    }
    if (m_bodyLogCooldown > 0) {
        --m_bodyLogCooldown;
        return;
    }
    if (std::fabs(m_currentAngle) > 0.5f) {
        m_bodyLogCooldown = 30; // ~0.5 s between progress lines while leaning
        LOGI("body lean cur=%.1f deg: head %.1f / torso %.1f / arms %.1f, lateral %.3f",
             static_cast<double>(m_currentAngle), static_cast<double>(headLeanDeg()),
             static_cast<double>(bodyLeanDeg()), static_cast<double>(armLeanDeg()),
             static_cast<double>(m_currentLateral));
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
    if (std::fabs(m_currentLateral) < 1e-5f && std::fabs(m_targetLateral) < 1e-5f)
        m_currentLateral = 0.f;

    // Body-part lean: same smoothed state, per-part degrees derived from
    // Max Lean Degrees via the part scales. (Renderer application comes in a
    // later batch; for now the values are computed and logged for verification.)
    if (m_bodyLeanEnabled) logBodyState();

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
    gb("bodyLeanEnabled", m_bodyLeanEnabled);
    gf("headScale", m_headScale);
    gf("bodyScale", m_bodyScale);
    gf("armScale", m_armScale);
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
}

} // namespace taczlean
