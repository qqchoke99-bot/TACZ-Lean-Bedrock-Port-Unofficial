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

// File-based probe log. Android blocks Termux (and every other app) from
// reading other apps' logcat, so logcat-only probing produced nothing on this
// device. The probe therefore writes directly to shared storage: Download
// first, then the launcher's games dir (the game can write there — worlds
// live there — and Termux can read it), then the app-specific external dir.
static const char* const kProbePaths[] = {
    "/sdcard/Download/taczlean_probe.log",
    "/sdcard/games/org.levimc/taczlean_probe.log",
    "/sdcard/Android/data/com.mojang.minecraftpe/files/taczlean_probe.log",
};
static int g_probePath = -1; // -1 undetermined, -2 none work

static FILE* probeOpen(const char* mode) {
    if (g_probePath == -2) return nullptr;
    if (g_probePath == -1) {
        constexpr int kPathCount =
            static_cast<int>(sizeof(kProbePaths) / sizeof(kProbePaths[0]));
        for (int i = 0; i < kPathCount; ++i) {
            if (FILE* t = std::fopen(kProbePaths[i], "a")) {
                std::fclose(t);
                g_probePath = i;
                break;
            }
        }
        if (g_probePath == -1) { g_probePath = -2; return nullptr; }
        if (FILE* m = std::fopen(kProbePaths[g_probePath], "a")) {
            std::fprintf(m, "[probe] using path: %s\n", kProbePaths[g_probePath]);
            std::fclose(m);
        }
    }
    return std::fopen(kProbePaths[g_probePath], mode);
}

static void probeFileLog(const char* fmt, ...) {
    FILE* f = probeOpen("a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

// Truncate and start a fresh log per session so stale addresses from
// previous runs don't pollute the analysis.
static void probeFileReset() {
    if (FILE* f = probeOpen("w")) {
        std::fprintf(f, "=== TaczLean session start ===\n");
        std::fclose(f);
    }
}

static void hexDumpToBuf(const unsigned char* p, std::size_t n,
                         char* out, std::size_t outCap) {
    const std::size_t maxi = (outCap - 1) / 2;
    if (n > maxi) n = maxi;
    for (std::size_t i = 0; i < n; ++i)
        std::snprintf(&out[i * 2], 3, "%02x", p[i]);
    out[n * 2] = '\0';
}

// ---- Batch-2 probe state ----
struct TrackedPart {
    void* addr;
    void* group; // pair2 (model instance container)
    char  name[16];
};
static std::vector<TrackedPart> g_trackedParts;
static std::vector<void*> g_snakeGroups; // groups containing left_arm/… parts
static std::uint64_t g_sampleCount = 0;
static bool g_worldSeen = false;
static bool g_writeProbeLogged = false;
static void* g_seenGroups[24] = {};
static int   g_seenGroupCount = 0;

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

    // Fresh file log per session: proves the mod loaded even if no parts
    // ever register, and drops stale data from previous runs.
    probeFileReset();
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
    // Reset batch-2/3 probe state so a re-enable starts a fresh capture.
    g_trackedParts.clear();
    g_snakeGroups.clear();
    g_sampleCount = 0;
    g_worldSeen = false;
    g_writeProbeLogged = false;
    g_seenGroupCount = 0;
}

void LeanModule::onRegisterPart(void* owner, void* pair, const char* name,
                                unsigned long long len, void* result) {
    if (!name || len == 0 || len > 15) return;
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

    void* pair2 = pair ? *reinterpret_cast<void* const*>(static_cast<const char*>(pair) + 8) : nullptr;
    void* factory = pair2 ? *reinterpret_cast<void* const*>(static_cast<const char*>(pair2) + 0x28) : nullptr;

    static std::uint64_t s_partCount = 0;
    ++s_partCount;

    // Full 0x220-byte object dump to the file log — reveals rotation blocks,
    // parent pointers, and everything else needed to pick safe write targets.
    char hex[0x220 * 2 + 2];
    hexDumpToBuf(static_cast<const unsigned char*>(owner), 0x220, hex, sizeof(hex));
    probeFileLog("part[%llu] '%.*s' owner=0x%lx pair2=0x%lx factory=0x%lx ret=0x%lx dump=%s",
                 static_cast<unsigned long long>(s_partCount), static_cast<int>(len), name,
                 reinterpret_cast<unsigned long>(owner),
                 reinterpret_cast<unsigned long>(pair2),
                 reinterpret_cast<unsigned long>(factory),
                 reinterpret_cast<unsigned long>(result), hex);

    LOGI("part[%llu] '%.*s' owner=0x%lx pair2=0x%lx",
         static_cast<unsigned long long>(s_partCount), static_cast<int>(len), name,
         reinterpret_cast<unsigned long>(owner),
         reinterpret_cast<unsigned long>(pair2));

    // Snapshot each unique group container (model instance) once.
    if (pair2) {
        bool seen = false;
        for (int i = 0; i < g_seenGroupCount; ++i)
            if (g_seenGroups[i] == pair2) { seen = true; break; }
        if (!seen &&
            g_seenGroupCount < static_cast<int>(sizeof(g_seenGroups) / sizeof(g_seenGroups[0]))) {
            g_seenGroups[g_seenGroupCount++] = pair2;
            char ghex[0x80 * 2 + 2];
            hexDumpToBuf(static_cast<const unsigned char*>(pair2), 0x80, ghex, sizeof(ghex));
            probeFileLog("group 0x%lx dump=%s",
                         reinterpret_cast<unsigned long>(pair2), ghex);
        }
    }

    // Track humanoid parts for the periodic pose sampler. snake_case names
    // (left_arm/…) belong to the 1.26 humanoid model; head/body are tracked
    // too (mob heads add comparison data). Cap the tracker for safety.
    bool tracked = std::memchr(name, '_', len) != nullptr;
    if (!tracked && len == 4)
        tracked = std::memcmp(name, "head", 4) == 0 || std::memcmp(name, "body", 4) == 0;
    if (tracked && g_trackedParts.size() < 96) {
        bool dup = false;
        for (const auto& tp : g_trackedParts)
            if (tp.addr == owner) { dup = true; break; }
        if (!dup) {
            TrackedPart tp{};
            tp.addr = owner;
            tp.group = pair2;
            std::memcpy(tp.name, name, len);
            tp.name[len] = '\0';
            g_trackedParts.push_back(tp);
        }
    }

    // Snake groups = the 1.26 humanoid model family (left_arm/… members).
    // The write probe touches only these; camelCase groups (villager-style)
    // and mob-head singletons stay untouched.
    if (std::memchr(name, '_', len) && pair2) {
        bool seen = false;
        for (void* g : g_snakeGroups)
            if (g == pair2) { seen = true; break; }
        if (!seen) g_snakeGroups.push_back(pair2);
    }

    // The sampling/write clock starts at the first part registration (world
    // join), not at mod load — otherwise the window burns down in the menu.
    if (!g_worldSeen) {
        g_worldSeen = true;
        s_bodyTickCount = 0;
        g_sampleCount = 0;
        probeFileLog("=== world join: sampling clock restarted ===");
    }
}

void LeanModule::onBodyTick() {
    ++s_bodyTickCount;
    // First call confirms the hook fires; afterwards keep it quiet (1 line
    // per ~2400 calls) so logcat stays readable.
    if (s_bodyTickCount == 1 || (s_bodyTickCount % 2400) == 0)
        LOGI("body tick alive, calls=%llu",
             static_cast<unsigned long long>(s_bodyTickCount));

    // Batch-6 probe: snapshots of the WHOLE object body (0x18..0x220) of
    // every tracked humanoid part, 2 Hz for 60 samples (~30 s). Fields that
    // change between samples are the live animation targets. Rounds 2-5 only
    // ever sampled 0x18..0x7F and saw zero organic movement while walking —
    // the live rotation storage must live beyond +0x80 (top candidate: the
    // identity quat vec4 at +0x20c, present in all 44 captured parts).
    if (!g_trackedParts.empty() && (s_bodyTickCount % 10) == 0 &&
        g_sampleCount < 60) {
        ++g_sampleCount;
        for (const auto& tp : g_trackedParts) {
            char hex[0x208 * 2 + 2];
            hexDumpToBuf(static_cast<const unsigned char*>(tp.addr) + 0x18,
                         0x208, hex, sizeof(hex));
            probeFileLog("s%02u '%s'@0x%lx g=0x%lx %s",
                         static_cast<unsigned>(g_sampleCount), tp.name,
                         reinterpret_cast<unsigned long>(tp.addr),
                         reinterpret_cast<unsigned long>(tp.group), hex);
        }
        if (g_sampleCount == 60)
            probeFileLog("=== sampling complete (60 samples) ===");
    }
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

    // Batch-6: LIVE-rotation candidate. Rounds 2-5 proved the renderer never
    // reads 0x18..0x7F for animation (zero organic movement while walking) —
    // the live pose must live beyond +0x80. Top structural candidate: the
    // identity quat vec4 at +0x20c, (0,0,0,1) in ALL 44 captured parts.
    // Compose the lean roll ONTO whatever rotation the part currently holds
    // (read → validate length → normalize → multiply → write back): body at
    // full angle, head/arms at half. If +0x20c is the live rotation, the
    // upper body visibly TILTS in third person. The +0x48 write below stays
    // so the first-person position effect is preserved during this test.
    // Legs stay planted.
    if (m_bodyLeanEnabled && !g_snakeGroups.empty()) {
        const float leanRad = m_currentAngle * 3.14159265f / 180.f;
        for (const auto& tp : g_trackedParts) {
            bool snakeGroup = false;
            for (void* g : g_snakeGroups)
                if (g == tp.group) { snakeGroup = true; break; }
            if (!snakeGroup) continue;
            if (std::strstr(tp.name, "leg")) continue;
            auto* p = reinterpret_cast<unsigned char*>(tp.addr);
            float k = m_bodyScale;
            if (std::strcmp(tp.name, "head") == 0) k = m_headScale * 0.5f;
            else if (std::strstr(tp.name, "arm"))  k = m_armScale  * 0.5f;

            // (a) +0x48 — proven visible effect (position-like slide);
            //     kept because the user wants the first-person movement.
            *reinterpret_cast<float*>(p + 0x48) = leanRad * k;

            // (b) +0x20c — live rotation quat candidate: roll composed onto
            //     the part's current rotation.
            auto* q = reinterpret_cast<float*>(p + 0x20c);
            Quat cur{q[0], q[1], q[2], q[3]};
            const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y +
                                         cur.z * cur.z + cur.w * cur.w);
            if (qlen > 0.5f && qlen < 1.5f) {
                cur = quatNormalize(cur);
                const float h = leanRad * k * 0.5f;
                const Quat rollQ{0.f, 0.f, std::sin(h), std::cos(h)};
                const Quat out = quatNormalize(quatMul(cur, rollQ));
                q[0] = out.x;
                q[1] = out.y;
                q[2] = out.z;
                q[3] = out.w;
            }
        }
        if (!g_writeProbeLogged) {
            g_writeProbeLogged = true;
            probeFileLog("write probe v4 active: parts=%zu angle=%.2f deg "
                         "(+0x48 kept, quat@0x20c composed)",
                         g_trackedParts.size(),
                         static_cast<double>(m_currentAngle));
        }
    }

    // Body-part lean: same smoothed state, per-part degrees derived from
    // Max Lean Degrees via the part scales. (Renderer application comes in a
    // later batch; for now the values are computed and logged for verification.)
    if (m_bodyLeanEnabled) logBodyState();

    auto* base = reinterpret_cast<char*>(cameraComponent);

    // --- Rotation quat @ +0x28 (proven stable) ---
    auto* qf = reinterpret_cast<float*>(base + kRotationOffset);
    Quat cur{qf[0], qf[1], qf[2], qf[3]};
    const float qlen = std::sqrt(cur.x * cur.x + cur.y * cur.y + cur.z * cur.z + cur.w * cur.w);
    // Relaxed bounds: camera shake may briefly store unnormalized quats;
    // normalize instead of bailing (bailing disabled the lean roll during
    // shake). Rejections are logged so the next session reveals what shake
    // actually writes.
    if (!(qlen > 0.05f && qlen < 20.f)) {
        static unsigned s_rejCount = 0;
        if ((s_rejCount++ % 120) == 0) {
            const auto* qb = reinterpret_cast<const unsigned*>(qf);
            probeFileLog("quat rejected #%u: len=%.5f raw=%08x %08x %08x %08x",
                         static_cast<unsigned>(s_rejCount),
                         static_cast<double>(qlen), qb[0], qb[1], qb[2], qb[3]);
        }
        return;
    }
    cur = quatNormalize(cur);

    // --- Batch-6 shake probe: did the roll we wrote last frame survive? ---
    // Extract the roll (twist around the view axis) from the incoming quat
    // and compare with the roll we applied. Zero quat rejections in the last
    // session proved this hook never bails — so the gun mod's camera shake
    // must overwrite the quat AFTER this hook. This log pins the wipe down
    // so the follow-up hook applies the roll after it.
    if (m_enableRoll && std::fabs(m_currentAngle) > 1.f) {
        static unsigned s_rollMismatch = 0;
        const float measuredDeg = 2.f * std::atan2(cur.z, cur.w) * 57.29578f;
        const float expectedDeg = m_currentAngle * m_firstPersonRollMult;
        if (std::fabs(measuredDeg - expectedDeg) > 3.f) {
            ++s_rollMismatch;
            if ((s_rollMismatch % 60) == 1)
                probeFileLog("roll wiped? expected %.1f deg, frame entry "
                             "%.1f deg (#%u)",
                             static_cast<double>(expectedDeg),
                             static_cast<double>(measuredDeg),
                             static_cast<unsigned>(s_rollMismatch));
        }
    }

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
