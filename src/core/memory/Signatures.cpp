#include <cameraoverhaul/memory/Signatures.hpp>

#include <array>
#include <string>
#include <vector>
#include <pl/memory/Signature.hpp>

namespace cameraoverhaul::memory {
namespace {
std::array<std::uintptr_t, SignatureCount> addresses{};
const std::array<SignatureDefinition, SignatureCount> definitions{{
    SignatureDefinition{SignatureId::NormalTick, "? ? ? FC ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D1 54 D0 3B D5 F3 03 00 AA ? ? ? F9 ? ? ? F8 ? ? ? 39"},
    SignatureDefinition{SignatureId::CameraBlendSystemTick, "? ? ? D1 ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? 6D ? ? ? A9 ? ? ? F9 ? ? ? A9 ? ? ? A9 ? ? ? 91 55 D0 3B D5 F3 03 01 AA F4 03 00 AA ? ? ? F9 ? ? ? 91 ? ? ? 91"},
}};
}

bool resolveAll(std::string_view libraryName) {
    const std::string library(libraryName);
    std::vector<std::string> patterns;
    patterns.reserve(definitions.size());
    for (const auto& definition : definitions) patterns.emplace_back(definition.pattern);
    const auto resolved = pl::memory::resolveSignatures(patterns, library.c_str());
    addresses.fill(0);
    bool any = false;
    for (std::size_t i = 0; i < definitions.size(); ++i) {
        const auto it = resolved.find(patterns[i]);
        if (it == resolved.end() || it->second == 0) continue;
        addresses[static_cast<std::size_t>(definitions[i].id)] = it->second;
        any = true;
    }
    return any;
}

std::uintptr_t resolve(SignatureId id) {
    const auto index = static_cast<std::size_t>(id);
    return index < addresses.size() ? addresses[index] : 0;
}

void clear() {
    addresses.fill(0);
}

}
