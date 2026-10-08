#include "launcher/Buttons.hpp"
#include "module/LeanModule.hpp"

#include <pl/ModMenu.hpp>

#include <android/log.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TaczLean", __VA_ARGS__)

namespace fs = std::filesystem;

namespace taczlean::buttons {
namespace {

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
            LOGI("loaded %s from %s (%zu bytes)", name, root.c_str(), data.size());
            return data;
        }
    }
    LOGI("asset not found: %s", name);
    return {};
}

// Q = Lean Left (Java/MC key Q)
void onLeft(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (event == E::Down) m.setLeanLeft(true);
    if (event == E::Up) m.setLeanLeft(false);
}

// E = Lean Right (Java/MC key E)
void onRight(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (event == E::Down) m.setLeanRight(true);
    if (event == E::Up) m.setLeanRight(false);
}

} // namespace

void registerAll() {
    auto& cfg = LeanModule::get();
    if (!cfg.m_showButtons) return;

    // Prefer Q/E names; fall back to L/R filenames if user still has old pack
    auto iconQ = loadAsset("button_iconQ.png");
    if (iconQ.empty()) iconQ = loadAsset("button_iconL.png");
    auto iconQPressed = loadAsset("button_icon_pressedQ.png");
    if (iconQPressed.empty()) iconQPressed = loadAsset("button_icon_pressedL.png");

    auto iconE = loadAsset("button_iconE.png");
    if (iconE.empty()) iconE = loadAsset("button_iconR.png");
    auto iconEPressed = loadAsset("button_icon_pressedE.png");
    if (iconEPressed.empty()) iconEPressed = loadAsset("button_icon_pressedR.png");

    // Q = Left
    {
        pl::modmenu::ButtonBuilder b{"taczlean.lean_left", "Lean Left (Q)"};
        b.moduleId(LeanModule::moduleId)
            .label("Q")
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onLeft);
        if (!iconQ.empty()) b.pngIcon(iconQ, true);
        if (!b.registerButton()) {
            LOGI("register lean_left (Q) failed");
        } else {
            LOGI("lean_left (Q) registered");
        }
    }

    // E = Right
    {
        pl::modmenu::ButtonBuilder b{"taczlean.lean_right", "Lean Right (E)"};
        b.moduleId(LeanModule::moduleId)
            .label("E")
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onRight);
        if (!iconE.empty()) b.pngIcon(iconE, true);
        if (!b.registerButton()) {
            LOGI("register lean_right (E) failed");
        } else {
            LOGI("lean_right (E) registered");
        }
    }
}

void unregisterAll() {
    pl::modmenu::unregisterButton("taczlean.lean_left");
    pl::modmenu::unregisterButton("taczlean.lean_right");
}

} // namespace taczlean::buttons
