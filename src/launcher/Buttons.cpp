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

void onConfigChanged(std::string_view /*moduleId*/, std::string_view key,
                     std::string_view value) {
    auto& m = LeanModule::get();
    const std::string k{key};
    const std::string v{value};
    try {
        if (k == "maxLeanDeg") m.m_maxLeanDeg = std::stof(v);
        else if (k == "lateralOffset") m.m_lateralOffset = std::stof(v);
        else if (k == "smoothFactor") m.m_smoothFactor = std::stof(v);
        else if (k == "holdMode") m.m_holdMode = (v == "true" || v == "1");
        else if (k == "enableLateral") m.m_enableLateral = (v == "true" || v == "1");
        else if (k == "enableRoll") m.m_enableRoll = (v == "true" || v == "1");
        else if (k == "bodyLeanEnabled") m.m_bodyLeanEnabled = (v == "true" || v == "1");
        else if (k == "headScale") m.m_headScale = std::stof(v);
        else if (k == "bodyScale") m.m_bodyScale = std::stof(v);
        else if (k == "armScale") m.m_armScale = std::stof(v);
        LOGI("config %s = %s", k.c_str(), v.c_str());
    } catch (...) {
    }
}

} // namespace

void registerAll() {
    auto& cfg = LeanModule::get();

    auto mb = pl::modmenu::ModuleBuilder(LeanModule::moduleId, LeanModule::name);
    mb.modId(kModId)
        .description("Lean: camera roll + lateral peek")
        .defaultEnabled(true)
        .onConfigChanged(onConfigChanged)
        .config("maxLeanDeg", "Max Lean Degrees",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_maxLeanDeg), "5", "45")
        .config("lateralOffset", "Lateral Offset (blocks)",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_lateralOffset), "0.05", "0.6")
        .config("smoothFactor", "Smooth Factor",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_smoothFactor), "0.05", "1.0")
        .config("holdMode", "Hold Mode (off=Toggle)",
                pl::modmenu::ConfigType::Toggle,
                cfg.m_holdMode ? "true" : "false")
        .config("enableLateral", "Enable Lateral Peek",
                pl::modmenu::ConfigType::Toggle,
                cfg.m_enableLateral ? "true" : "false")
        .config("enableRoll", "Enable Camera Roll",
                pl::modmenu::ConfigType::Toggle,
                cfg.m_enableRoll ? "true" : "false")
        .config("bodyLeanEnabled", "Body Part Lean (head/torso/arms)",
                pl::modmenu::ConfigType::Toggle,
                cfg.m_bodyLeanEnabled ? "true" : "false")
        .config("headScale", "Body: Head Scale",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_headScale), "0.0", "2.0")
        .config("bodyScale", "Body: Torso Scale",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_bodyScale), "0.0", "2.0")
        .config("armScale", "Body: Arms Scale",
                pl::modmenu::ConfigType::SliderFloat,
                std::to_string(cfg.m_armScale), "0.0", "2.0");
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
            .label("C")
            .androidKeyCode(0)
            .behavior(behavior)
            .defaultVisible(true)
            .onEvent(onLeft);
        if (!iconLeft.empty()) b.pngIcon(iconLeft, true);
        LOGI("btn C (lean left) -> %s", b.registerButton() ? "OK" : "FAIL");
    }
    {
        auto b = pl::modmenu::ButtonBuilder("taczlean.lean_right", "Lean Right");
        b.moduleId(LeanModule::moduleId)
            .modId(kModId)
            .label("Z")
            .androidKeyCode(0)
            .behavior(behavior)
            .defaultVisible(true)
            .onEvent(onRight);
        if (!iconRight.empty()) b.pngIcon(iconRight, true);
        LOGI("btn Z (lean right) -> %s", b.registerButton() ? "OK" : "FAIL");
    }
}

void unregisterAll() {
    pl::modmenu::unregisterButton("taczlean.lean_left");
    pl::modmenu::unregisterButton("taczlean.lean_right");
    pl::modmenu::unregisterModule(LeanModule::moduleId);
}

} // namespace taczlean::buttons
