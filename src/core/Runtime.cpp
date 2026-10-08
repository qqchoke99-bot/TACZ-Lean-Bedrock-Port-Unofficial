#include "core/Runtime.hpp"
#include "module/LeanModule.hpp"
#include "launcher/Buttons.hpp"

#include <cameraoverhaul/memory/Signatures.hpp>
#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TaczLean", __VA_ARGS__)

namespace taczlean {

Runtime& Runtime::get() {
    static Runtime r;
    return r;
}

bool Runtime::load(pl::mod::ModContext&) {
    LOGI("load");
    return true;
}

bool Runtime::enable(pl::mod::ModContext&) {
    LOGI("enable");
    cameraoverhaul::memory::resolveAll("libminecraftpe.so");
    LeanModule::get().init();
    buttons::registerAll();
    return true;
}

bool Runtime::disable(pl::mod::ModContext&) {
    LOGI("disable");
    buttons::unregisterAll();
    LeanModule::get().shutdown();
    return true;
}

bool Runtime::unload(pl::mod::ModContext&) {
    return true;
}

}
