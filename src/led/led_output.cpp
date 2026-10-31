#include "led/led_output.h"

#include <FastLED.h>
#include <algorithm>
#include <cmath>

namespace ledlab {
namespace {

CRGB physicalPixels[config::LED_COUNT];

float channel(const float value) {
    return std::isfinite(value) ? std::max(0.0f, std::min(value, 1.0f)) : 0.0f;
}

}  // namespace

bool LedOutput::begin() {
    FastLED.addLeds<WS2812B, config::LED_DATA_PIN, GRB>(physicalPixels, config::LED_COUNT);
    FastLED.setBrightness(255);
    for (uint16_t i = 0; i < config::LED_COUNT; ++i) physicalPixels[i] = CRGB::Black;
    FastLED.show();
    initialized_ = true;
    return true;
}

void LedOutput::clear() {
    if (!initialized_) return;
    fill_solid(physicalPixels, config::LED_COUNT, CRGB::Black);
    const uint32_t started = micros();
    FastLED.show();
    transferTimeUs_ = micros() - started;
    estimatedCurrentMa_ = 0;
    effectiveBrightness_ = 0;
    powerLimited_ = false;
}

void LedOutput::write(const std::array<Rgb, config::LED_COUNT>& frame,
                      const DeviceSettings& settings) {
    if (!initialized_) return;
    const float brightness = std::min<uint8_t>(settings.brightness, 100) / 100.0f;
    const float idleCurrent = static_cast<float>(config::LED_COUNT);
    float dynamicCurrent = 0;
    for (const Rgb& pixel : frame) {
        dynamicCurrent += (channel(pixel.r) + channel(pixel.g) + channel(pixel.b)) *
                          brightness * config::LED_ESTIMATED_MA_PER_CHANNEL;
    }

    const uint16_t budget = std::max<uint16_t>(config::CURRENT_LIMIT_MA_MIN, std::min<uint16_t>(
        settings.currentLimitMa, config::CURRENT_LIMIT_MA_MAX));
    const float availableCurrent = std::max(0.0f, static_cast<float>(budget) - idleCurrent);
    const float powerScale = dynamicCurrent > availableCurrent && dynamicCurrent > 0
                                 ? availableCurrent / dynamicCurrent
                                 : 1.0f;
    const float outputScale = brightness * powerScale;
    powerLimited_ = powerScale < 0.999f;
    effectiveBrightness_ = static_cast<uint8_t>(std::lround(
        std::min(1.0f, brightness * powerScale) * 100.0f));
    currentLimitMa_ = budget;
    estimatedCurrentMa_ = static_cast<uint16_t>(std::min<float>(
        budget, idleCurrent + dynamicCurrent * powerScale));

    for (uint16_t i = 0; i < config::LED_COUNT; ++i) {
        physicalPixels[i].r = static_cast<uint8_t>(std::lround(channel(frame[i].r) * outputScale * 255));
        physicalPixels[i].g = static_cast<uint8_t>(std::lround(channel(frame[i].g) * outputScale * 255));
        physicalPixels[i].b = static_cast<uint8_t>(std::lround(channel(frame[i].b) * outputScale * 255));
    }
    const uint32_t started = micros();
    FastLED.show();
    transferTimeUs_ = micros() - started;
}

}  // namespace ledlab
