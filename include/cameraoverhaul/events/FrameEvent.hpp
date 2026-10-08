#pragma once

#include <cameraoverhaul/events/Event.hpp>

namespace cameraoverhaul::events {

struct FrameEvent {
    static constexpr EventType type = EventType::Frame;
};

}
