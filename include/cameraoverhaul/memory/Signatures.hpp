#pragma once

#include <cameraoverhaul/Export.hpp>
#include <cstdint>
#include <string_view>

namespace cameraoverhaul::memory {

enum class SignatureId : std::uint32_t {
    NormalTick,
    CameraBlendSystemTick,
    Count
};

inline constexpr std::size_t SignatureCount = static_cast<std::size_t>(SignatureId::Count);

struct SignatureDefinition {
    SignatureId id;
    std::string_view pattern;
};

CAMERAOVERHAUL_API bool resolveAll(std::string_view libraryName = "libminecraftpe.so");
CAMERAOVERHAUL_API std::uintptr_t resolve(SignatureId id);
CAMERAOVERHAUL_API void clear();

}
