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
        if (!data.empty()) return data;
    }
    return {};
}

void onLeft(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (m.m_holdMode) {
        if (event == E::Down) m.onButtonLeft(true);
        if (event == E::Up) m.onButtonLeft(false);
    } else {
        // Toggle: fire on Down only
        if (event == E::Down) m.onButtonLeft(true);
    }
}

void onRight(std::string_view, pl::modmenu::ButtonEvent event, float) {
    using E = pl::modmenu::ButtonEvent;
    auto& m = LeanModule::get();
    if (m.m_holdMode) {
        if (event == E::Down) m.onButtonRight(true);
        if (event == E::Up) m.onButtonRight(false);
    } else {
        if (event == E::Down) m.onButtonRight(true);
    }
}

} // namespace

void registerAll() {
    auto& cfg = LeanModule::get();

    // Module + settings in Levi Mod Menu
    auto mb = pl::modmenu::ModuleBuilder(LeanModule::moduleId, LeanModule::name);
    mb.modId(kModId)
        .description("Lean: camera roll + lateral peek")
        .defaultEnabled(true)
        .config("maxLeanDeg", "Max Lean Degrees", pl::modmenu::ConfigType::Slider,
                std::to_string(cfg.m_maxLeanDeg), "5", "45")
        .config("lateralOffset", "Lateral Offset (blocks)", pl::modmenu::ConfigType::Slider,
                std::to_string(cfg.m_lateralOffset), "0.05", "0.6")
        .config("smoothFactor", "Smooth Factor", pl::modmenu::ConfigType::Slider,
                std::to_string(cfg.m_smoothFactor), "0.05", "1.0")
        .config("holdMode", "Hold Mode (off=Toggle)", pl::modmenu::ConfigType::Toggle,
                cfg.m_holdMode ? "true" : "false")
        .config("enableLateral", "Enable Lateral Peek", pl::modmenu::ConfigType::Toggle,
                cfg.m_enableLateral ? "true" : "false")
        .config("enableRoll", "Enable Camera Roll", pl::modmenu::ConfigType::Toggle,
                cfg.m_enableRoll ? "true" : "false");
    LOGI("registerModule -> %s", mb.registerModule() ? "OK" : "FAIL");

    if (!cfg.m_showButtons) return;

    auto iconLeft = loadAsset("button_iconQ.png");
    if (iconLeft.empty()) iconLeft = loadAsset("button_iconL.png");
    auto iconRight = loadAsset("button_iconE.png");
    if (iconRight.empty()) iconRight = loadAsset("button_iconR.png");

    const auto behavior = cfg.m_holdMode ? pl::modmenu::ButtonBehavior::Hold
                                         : pl::modmenu::ButtonBehavior::Toggle;

    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_left", "Lean Left");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("Z")
            .androidKeyCode(0)
            .behavior(behavior)
            .defaultVisible(true)
            .onEvent(onLeft);
        if (!iconLeft.empty()) b.pngIcon(iconLeft, true);
        LOGI("btn Z -> %s", b.registerButton() ? "OK" : "FAIL");
    }
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_right", "Lean Right");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("C")
            .androidKeyCode(0)
            .behavior(behavior)
            .defaultVisible(true)
            .onEvent(onRight);
        if (!iconRight.empty()) b.pngIcon(iconRight, true);
        LOGI("btn C -> %s", b.registerButton() ? "OK" : "FAIL");
    }
}

void unregisterAll() {
    pl::modmenu::unregisterButton("taczlean.lean_left");
    pl::modmenu::unregisterButton("taczlean.lean_right");
    pl::modmenu::unregisterModule(LeanModule::moduleId);
}

} // namespace taczlean::buttons
