#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <functional>
#include <mutex>

#include "led/led_output.h"
#include "runtime/animation_runtime.h"
#include "system/device_state.h"
#include "system/wifi_credentials.h"

namespace ledlab {

class WebServerApp {
public:
    WebServerApp(AnimationRuntime& runtime, LedOutput& leds,
                 DeviceSettings& settings, DeviceMetrics& metrics,
                 WifiCredentials& wifiCredentials, WifiDiagnostics& wifiDiagnostics);
    bool begin();
    bool takeStepRequest();
    bool takeOutputRefreshRequest();
    bool takeClearRequest();
    std::mutex& stateMutex() { return stateMutex_; }

private:
    using JsonPostHandler =
        std::function<void(AsyncWebServerRequest*, const String&)>;

    void registerRoutes();
    void registerJsonPost(const char* path, size_t maxBodySize,
                          JsonPostHandler handler);
    void resumeHomeWifi();
    void sendError(AsyncWebServerRequest* request, uint16_t status,
                   const char* code, const String& message,
                   uint16_t line = 0, uint16_t column = 0);
    void sendJson(AsyncWebServerRequest* request, uint16_t status,
                  JsonDocument& document);
    void serveStatic(AsyncWebServerRequest* request);

    AsyncWebServer server_;
    AnimationRuntime& runtime_;
    LedOutput& leds_;
    DeviceSettings& settings_;
    DeviceMetrics& metrics_;
    WifiCredentials& wifiCredentials_;
    WifiDiagnostics& wifiDiagnostics_;
    std::mutex stateMutex_;
    bool stepRequested_ = false;
    bool outputRefreshRequested_ = false;
    bool clearRequested_ = false;
    bool wifiScanActive_ = false;
    bool wifiScanResumeStation_ = false;
};

}  // namespace ledlab
