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
            LOGI("loaded %s (%zu bytes)", name, data.size());
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

    const bool modOk =
        pl::modmenu::ModuleBuilder(LeanModule::moduleId, LeanModule::name)
            .modId(kModId)
            .description("Hold Z/C on-screen to lean (no game keybind)")
            .defaultEnabled(true)
            .registerModule();
    LOGI("registerModule %s -> %s", LeanModule::moduleId, modOk ? "OK" : "FAIL");

    if (!cfg.m_showButtons) return;

    // Optional icons (still load Q/E art as lean glyphs if present)
    auto iconLeft = loadAsset("button_iconQ.png");
    if (iconLeft.empty()) iconLeft = loadAsset("button_iconL.png");
    if (iconLeft.empty()) iconLeft = loadAsset("button_iconZ.png");

    auto iconRight = loadAsset("button_iconE.png");
    if (iconRight.empty()) iconRight = loadAsset("button_iconR.png");
    if (iconRight.empty()) iconRight = loadAsset("button_iconC.png");

    // Z = Lean Left — no androidKeyCode (0) so ไม่ชนปุ่มเกม
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_left", "Lean Left");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("Z")
            .androidKeyCode(0)
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onLeft);
        if (!iconLeft.empty()) b.pngIcon(iconLeft, true);
        LOGI("registerButton Z (left) -> %s", b.registerButton() ? "OK" : "FAIL");
    }

    // C = Lean Right
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_right", "Lean Right");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("C")
            .androidKeyCode(0)
            .behavior(pl::modmenu::ButtonBehavior::Hold)
            .defaultVisible(true)
            .onEvent(onRight);
        if (!iconRight.empty()) b.pngIcon(iconRight, true);
        LOGI("registerButton C (right) -> %s", b.registerButton() ? "OK" : "FAIL");
    }
}

void unregisterAll() {
    pl::modmenu::unregisterButton("taczlean.lean_left");
    pl::modmenu::unregisterButton("taczlean.lean_right");
    pl::modmenu::unregisterModule(LeanModule::moduleId);
}

} // namespace taczlean::buttons
