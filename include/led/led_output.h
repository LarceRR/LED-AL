#pragma once

#include <array>

#include "runtime/animation_runtime.h"
#include "system/device_state.h"

namespace ledlab {

class LedOutput {
public:
    bool begin();
    void clear();
    void write(const std::array<Rgb, config::LED_COUNT>& frame,
               const DeviceSettings& settings);

    uint16_t estimatedCurrentMa() const { return estimatedCurrentMa_; }
    uint8_t effectiveBrightness() const { return effectiveBrightness_; }
    uint16_t currentLimitMa() const { return currentLimitMa_; }
    bool powerLimited() const { return powerLimited_; }
    uint32_t transferTimeUs() const { return transferTimeUs_; }

private:
    uint16_t estimatedCurrentMa_ = 0;
    uint16_t currentLimitMa_ = config::CURRENT_LIMIT_MA_DEFAULT;
    uint8_t effectiveBrightness_ = config::LED_BRIGHTNESS_DEFAULT;
    uint32_t transferTimeUs_ = 0;
    bool initialized_ = false;
    bool powerLimited_ = false;
};

}  // namespace ledlab
