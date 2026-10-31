#pragma once

#include <Arduino.h>

namespace ledlab::config {

constexpr uint16_t LED_COUNT = 300;
constexpr uint8_t LED_DATA_PIN = 23;
constexpr uint8_t LED_BRIGHTNESS_DEFAULT = 20;
constexpr uint16_t CURRENT_LIMIT_MA_DEFAULT = 2000;
constexpr uint16_t CURRENT_LIMIT_MA_MIN = 500;
constexpr uint16_t CURRENT_LIMIT_MA_MAX = 18000;
constexpr uint16_t LED_ESTIMATED_MA_PER_CHANNEL = 20;

constexpr char WIFI_SSID[] = "LED-LAB";
constexpr char WIFI_PASSWORD[] = "ledlab123";
constexpr char WIFI_IP[] = "192.168.4.1";
constexpr char WIFI_GATEWAY[] = "192.168.4.1";
constexpr char WIFI_SUBNET[] = "255.255.255.0";
constexpr uint16_t HTTP_PORT = 80;

constexpr size_t MAX_SOURCE_BYTES = 16384;
constexpr uint32_t MAX_TOKENS = 4096;
constexpr uint32_t MAX_AST_NODES = 2048;
constexpr uint16_t MAX_STATE_VARIABLES = 32;
constexpr uint16_t MAX_ARRAY_LENGTH = 512;
constexpr uint16_t MAX_LOOP_ITERATIONS = 10000;
constexpr uint16_t MAX_EXPRESSION_DEPTH = 64;
constexpr uint16_t MAX_RUNTIME_LOCALS = 128;
constexpr uint8_t MAX_BUILTIN_ARGUMENTS = 32;
constexpr uint32_t MAX_INSTRUCTIONS_PER_FRAME = 300000;
constexpr uint32_t MAX_INIT_INSTRUCTIONS = 10000;
constexpr uint32_t MAX_FRAME_TIME_US = 80000;
constexpr uint32_t MAX_COMPILATION_TIME_US = 200000;
constexpr uint16_t TARGET_FPS_DEFAULT = 60;
constexpr uint8_t MAX_ANIMATION_NAME_BYTES = 64;

}  // namespace ledlab::config
