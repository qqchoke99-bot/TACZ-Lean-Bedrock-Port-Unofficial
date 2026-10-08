#include "core/Runtime.hpp"
#include <pl/Mod.hpp>

class TaczLeanMod {
public:
    static TaczLeanMod& instance() {
        static TaczLeanMod m;
        return m;
    }
    bool load(pl::mod::ModContext& c) { return taczlean::Runtime::get().load(c); }
    bool enable(pl::mod::ModContext& c) { return taczlean::Runtime::get().enable(c); }
    bool disable(pl::mod::ModContext& c) { return taczlean::Runtime::get().disable(c); }
    bool unload(pl::mod::ModContext& c) { return taczlean::Runtime::get().unload(c); }
};

PL_REGISTER_MOD(TaczLeanMod, TaczLeanMod::instance())
