#pragma once

#if defined(__GNUC__) || defined(__clang__)
#define CAMERAOVERHAUL_API __attribute__((visibility("default")))
#else
#define CAMERAOVERHAUL_API
#endif
