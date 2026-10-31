#pragma once

#include <stdint.h>
#include <string>

#include "device_config.h"

namespace ledlab {

struct DeviceSettings {
    uint8_t brightness = config::LED_BRIGHTNESS_DEFAULT;
    float timeScale = 1.0f;
    uint16_t targetFps = config::TARGET_FPS_DEFAULT;
    uint16_t currentLimitMa = config::CURRENT_LIMIT_MA_DEFAULT;
};

struct DeviceMetrics {
    float actualFps = 0;
    float frameTimeMs = 0;
    float transferTimeMs = 0;
    uint32_t missedFrames = 0;
    std::string lastErrorCode;
    std::string lastErrorMessage;
    uint16_t lastErrorLine = 0;
    uint16_t lastErrorColumn = 0;
};

}  // namespace ledlab
