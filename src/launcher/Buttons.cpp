#include "launcher/Buttons.hpp"
#include "module/LeanModule.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TaczLean", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "TaczLean", __VA_ARGS__)

namespace fs = std::filesystem;

namespace taczlean::buttons {
namespace {

// Must match levimod.json / package name used by launcher
constexpr const char* kModId = "TaczLean";

std::vector<unsigned char> readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {});
}

std::vector<fs::path> assetRoots() {
    return {
        "/sdcard/games/TaczLean/buttons",
        "/storage/emulated/0/games/TaczLean/buttons",
        "/storage/emulated/0/Android/media/org.levimc.launcher/TaczLean/buttons",
        "/sdcard/Android/media/org.levimc.launcher/TaczLean/buttons",
    };
}

std::vector<unsigned char> loadAsset(const char* name) {
    for (const auto& root : assetRoots()) {
        auto data = readFile(root / name);
        if (!data.empty()) {
            LOGI("loaded %s (%zu bytes) from %s", name, data.size(), root.c_str());
            return data;
        }
    }
    return {};
}

void onLeft(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (event == E::Down) m.setLeanLeft(true);
    if (event == E::Up) m.setLeanLeft(false);
}

void onRight(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (event == E::Down) m.setLeanRight(true);
    if (event == E::Up) m.setLeanRight(false);
}

} // namespace

void registerAll() {
    auto& cfg = LeanModule::get();

    // 1) Register module FIRST (required before floating buttons)
    const bool modOk =
        pl::modmenu::ModuleBuilder(LeanModule::moduleId, LeanModule::name)
            .modId(kModId)
            .description("Hold Q/E on-screen to lean camera")
            .defaultEnabled(true)
            .registerModule();
    LOGI("registerModule %s -> %s", LeanModule::moduleId, modOk ? "OK" : "FAIL");

    if (!cfg.m_showButtons) {
        LOGI("showButtons=false, skip buttons");
        return;
    }

    auto iconQ = loadAsset("button_iconQ.png");
    if (iconQ.empty()) iconQ = loadAsset("button_iconL.png");
    auto iconE = loadAsset("button_iconE.png");
    if (iconE.empty()) iconE = loadAsset("button_iconR.png");

    // 2) Q = Lean Left — floating Hold button
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_left", "Lean Left (Q)");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("Q")
            .androidKeyCode(45) // KEYCODE_Q
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onLeft);
        if (!iconQ.empty()) b.pngIcon(iconQ, /*keepAspect=*/true);
        const bool ok = b.registerButton();
        LOGI("registerButton Q (left) -> %s", ok ? "OK" : "FAIL");
        if (!ok) LOGE("ButtonBuilder lean_left failed — check moduleId/modId");
    }

    // 3) E = Lean Right
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_right", "Lean Right (E)");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("E")
            .androidKeyCode(33) // KEYCODE_E
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onRight);
        if (!iconE.empty()) b.pngIcon(iconE, /*keepAspect=*/true);
        const bool ok = b.registerButton();
        LOGI("registerButton E (right) -> %s", ok ? "OK" : "FAIL");
        if (!ok) LOGE("ButtonBuilder lean_right failed");
    }
}

void unregisterAll() {
    pl::modmenu::unregisterButton("taczlean.lean_left");
    pl::modmenu::unregisterButton("taczlean.lean_right");
    pl::modmenu::unregisterModule(LeanModule::moduleId);
}

} // namespace taczlean::buttons
