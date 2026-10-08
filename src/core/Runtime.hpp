#pragma once
#include <pl/Mod.hpp>

namespace taczlean {
class Runtime {
public:
    static Runtime& get();
    bool load(pl::mod::ModContext& ctx);
    bool enable(pl::mod::ModContext& ctx);
    bool disable(pl::mod::ModContext& ctx);
    bool unload(pl::mod::ModContext& ctx);
};
}
