#pragma once

#include <cameraoverhaul/events/Event.hpp>

namespace cameraoverhaul::sdk { class Player; }

namespace cameraoverhaul::events {

struct LocalPlayerTickEvent {
    static constexpr EventType type = EventType::LocalPlayerTick;
    sdk::Player* player;
};

}
