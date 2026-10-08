#pragma once

#include <cstdint>

namespace cameraoverhaul::events {

enum class EventType : std::uint32_t {
    Frame = 1,
    LocalPlayerTick
};

enum class EventPriority : std::int32_t {
    First = 200,
    Early = 100,
    Normal = 0,
    Late = -100,
    Last = -200
};

}
