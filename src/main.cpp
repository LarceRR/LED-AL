#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include <esp_heap_caps.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "device_config.h"
#include "led/led_output.h"
#include "runtime/animation_runtime.h"
#include "system/device_state.h"
#include "system/wifi_credentials.h"
#include "system/wifi_manager.h"
#include "version.h"
#include "web/web_server.h"

namespace {

ledlab::AnimationRuntime runtime;
ledlab::LedOutput leds;
ledlab::DeviceSettings settings;
ledlab::DeviceMetrics metrics;
ledlab::WifiCredentials wifiCredentials;
ledlab::WifiDiagnostics wifiDiagnostics;
ledlab::WifiManager wifiManager(wifiCredentials, wifiDiagnostics);
ledlab::WebServerApp web(runtime, leds, settings, metrics, wifiCredentials,
                         wifiDiagnostics, wifiManager);
std::array<ledlab::Rgb, ledlab::config::LED_COUNT> frameBuffer{};

void logLine(const char* level, const char* message) {
    Serial.printf("{\"level\":\"%s\",\"component\":\"system\",\"message\":\"%s\"}\n",
                  level, message);
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_START:
            wifiDiagnostics.state.store(ledlab::WifiConnectionState::Connecting);
            Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"station_started\"}\n");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            wifiDiagnostics.state.store(ledlab::WifiConnectionState::Connecting);
            Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"associated\",\"channel\":%u,\"authmode\":%u,\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\"}\n",
                          info.wifi_sta_connected.channel,
                          static_cast<unsigned>(info.wifi_sta_connected.authmode),
                          info.wifi_sta_connected.bssid[0],
                          info.wifi_sta_connected.bssid[1],
                          info.wifi_sta_connected.bssid[2],
                          info.wifi_sta_connected.bssid[3],
                          info.wifi_sta_connected.bssid[4],
                          info.wifi_sta_connected.bssid[5]);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            wifiDiagnostics.state.store(ledlab::WifiConnectionState::Connected);
            wifiDiagnostics.disconnectReason.store(0);
            wifiDiagnostics.disconnectRssi.store(0);
            Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"got_ip\",\"ip\":\"%s\"}\n",
                          WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            const uint8_t reason = info.wifi_sta_disconnected.reason;
            wifiDiagnostics.disconnectReason.store(reason);
            wifiDiagnostics.disconnectRssi.store(info.wifi_sta_disconnected.rssi);
            wifiDiagnostics.state.store(ledlab::WifiConnectionState::Disconnected);
            Serial.printf("{\"level\":\"error\",\"component\":\"wifi\",\"event\":\"disconnected\",\"reason\":%u,\"rssi\":%d}\n",
                          reason, info.wifi_sta_disconnected.rssi);
            break;
        }
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            wifiDiagnostics.state.store(ledlab::WifiConnectionState::Disconnected);
            Serial.printf("{\"level\":\"warning\",\"component\":\"wifi\",\"event\":\"lost_ip\"}\n");
            break;
        default:
            break;
    }
}

struct LegacyWifiCredentials {
    char ssid[33]{};
    char password[64]{};
};

bool profileIsValid(const ledlab::WifiNetworkProfile& profile) {
    return profile.ssid[0] != '\0' &&
           std::memchr(profile.ssid, '\0', sizeof(profile.ssid)) != nullptr &&
           std::memchr(profile.password, '\0', sizeof(profile.password)) != nullptr;
}

bool loadWifiCredentials() {
    Preferences preferences;
    if (!preferences.begin("ledlab", false)) {
        logLine("error", "Could not open Wi-Fi credential storage");
        return false;
    }

    const size_t storedLength = preferences.getBytesLength("wifi");
    bool loaded = false;
    bool migrated = false;

    if (storedLength == sizeof(ledlab::WifiCredentials)) {
        ledlab::WifiCredentials stored{};
        if (preferences.getBytes("wifi", &stored, sizeof(stored)) == sizeof(stored)) {
            ledlab::WifiCredentials normalized{};
            size_t destination = 0;
            for (const auto& profile : stored.networks) {
                if (destination >= 3) break;
                if (!profileIsValid(profile)) continue;
                normalized.networks[destination++] = profile;
            }
            wifiCredentials = normalized;
            loaded = wifiCredentials.networks[0].ssid[0] != '\0';
        }
    } else if (storedLength == sizeof(LegacyWifiCredentials)) {
        LegacyWifiCredentials legacy{};
        if (preferences.getBytes("wifi", &legacy, sizeof(legacy)) == sizeof(legacy) &&
            legacy.ssid[0] != '\0' &&
            std::memchr(legacy.ssid, '\0', sizeof(legacy.ssid)) != nullptr &&
            std::memchr(legacy.password, '\0', sizeof(legacy.password)) != nullptr) {
            std::memcpy(wifiCredentials.networks[0].ssid, legacy.ssid,
                        sizeof(legacy.ssid));
            std::memcpy(wifiCredentials.networks[0].password, legacy.password,
                        sizeof(legacy.password));
            loaded = true;
            migrated = true;
        }
    }

    if (loaded && (migrated ||
        storedLength != sizeof(ledlab::WifiCredentials))) {
        const size_t written = preferences.putBytes(
            "wifi", &wifiCredentials, sizeof(wifiCredentials));
        if (written != sizeof(wifiCredentials)) {
            preferences.end();
            logLine("error", "Could not migrate Wi-Fi credentials to the multi-network format");
            return false;
        }
    }
    preferences.end();

    if (loaded) {
        uint8_t profileCount = 0;
        for (const auto& profile : wifiCredentials.networks) {
            if (profile.ssid[0] != '\0') ++profileCount;
        }
        Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"message\":\"Loaded saved home network profiles\",\"count\":%u,\"primarySsid\":\"%s\"}\n",
                      profileCount, wifiCredentials.networks[0].ssid);
    } else if (storedLength != 0) {
        logLine("warning", "Saved Wi-Fi credentials were invalid or used an unsupported format");
    }
    return loaded;
}

}  // namespace

// The DSL interpreter uses bounded recursion; 16 KiB leaves headroom for
// nested expressions while the frame staging buffer itself lives outside the stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.printf("\nLED LAB %s starting\n", LEDLAB_FIRMWARE_VERSION);
    WiFi.onEvent(onWifiEvent);

    if (!leds.begin()) {
        logLine("error", "LED driver initialization failed");
    } else {
        logLine("info", "WS2812B output initialized black");
    }

    loadWifiCredentials();
    if (!wifiManager.begin()) {
        logLine("error", "Wi-Fi manager initialization failed");
    }
    const bool filesystemReady = LittleFS.begin(false, "/littlefs", 10, "littlefs");
    if (!filesystemReady) {
        logLine("warning", "LittleFS mount failed; optional filesystem assets are unavailable, but the embedded web app remains available");
    } else {
        Serial.printf("{\"level\":\"info\",\"component\":\"filesystem\",\"totalBytes\":%lu,\"usedBytes\":%lu}\n",
                      static_cast<unsigned long>(LittleFS.totalBytes()),
                      static_cast<unsigned long>(LittleFS.usedBytes()));
    }
    runtime.setTimeScale(settings.timeScale);
    if (!web.begin(filesystemReady)) {
        logLine("error", "HTTP server initialization failed");
    } else {
        logLine("info", "HTTP server started on port 80");
    }
    logLine("info", "Runtime ready; no animation is running");
}

void loop() {
    std::lock_guard<std::mutex> stateLock(web.stateMutex());
    wifiManager.service();
    runtime.serviceCompile();

    static uint32_t lastJobId = 0;
    static uint32_t lastFrameUs = 0;
    static uint32_t nextFrameUs = 0;
    static uint32_t fpsWindowStartMs = millis();
    static uint32_t fpsFrameCount = 0;

    const ledlab::RuntimeJob& job = runtime.job();
    if (job.id != lastJobId && job.complete && job.success) {
        lastJobId = job.id;
        metrics.lastErrorCode.clear();
        metrics.lastErrorMessage.clear();
        metrics.lastErrorLine = 0;
        metrics.lastErrorColumn = 0;
        const uint32_t interval = 1000000UL / settings.targetFps;
        const uint32_t started = micros();
        lastFrameUs = started - interval;
        nextFrameUs = started;
        Serial.printf("{\"level\":\"info\",\"component\":\"runtime\",\"message\":\"Animation started\",\"name\":\"%s\"}\n",
                      job.name.c_str());
    }

    const bool step = web.takeStepRequest();
    const bool refreshOutput = web.takeOutputRefreshRequest();
    if (web.takeClearRequest()) {
        frameBuffer.fill(ledlab::Rgb{});
        leds.clear();
    }
    if (refreshOutput && !runtime.running()) {
        leds.write(frameBuffer, settings);
    }
    const uint32_t now = micros();
    const uint32_t intervalUs = 1000000UL / settings.targetFps;
    if (step || (runtime.running() &&
                 static_cast<int32_t>(now - nextFrameUs) >= 0)) {
        const uint32_t elapsedUs = lastFrameUs == 0 ? intervalUs : now - lastFrameUs;
        const uint32_t frameStartedUs = now;
        const float deltaTime = std::min(0.05f, elapsedUs / 1000000.0f);
        ledlab::RuntimeError error;
        if (runtime.renderFrame(deltaTime, frameBuffer, error, step)) {
            leds.write(frameBuffer, settings);
            const uint32_t totalUs = runtime.lastComputeTimeUs() + leds.transferTimeUs();
            metrics.frameTimeMs = totalUs / 1000.0f;
            metrics.transferTimeMs = leds.transferTimeUs() / 1000.0f;
            if (!step) ++fpsFrameCount;
            if (!step && elapsedUs > intervalUs + intervalUs / 2) {
                metrics.missedFrames += elapsedUs / intervalUs - 1;
            }
            if (!step) {
                lastFrameUs = frameStartedUs;
                nextFrameUs = frameStartedUs + intervalUs;
            }
        } else {
            metrics.lastErrorCode = error.code;
            metrics.lastErrorMessage = error.message;
            metrics.lastErrorLine = error.line;
            metrics.lastErrorColumn = error.column;
            Serial.printf("{\"level\":\"error\",\"component\":\"runtime\",\"code\":\"%s\",\"message\":\"%s\",\"line\":%u,\"column\":%u}\n",
                          error.code.c_str(), error.message.c_str(), error.line, error.column);
        }
    }

    const uint32_t nowMs = millis();
    static uint32_t lastMemoryDiagnosticsMs = 0;
    if (nowMs - lastMemoryDiagnosticsMs >= 10000) {
        lastMemoryDiagnosticsMs = nowMs;
        Serial.printf("{\"level\":\"debug\",\"component\":\"runtime\",\"stackHighWater\":%u,\"freeHeap\":%u,\"largestFreeBlock\":%u}\n",
                      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    }
    if (nowMs - fpsWindowStartMs >= 1000) {
        metrics.actualFps = fpsFrameCount * 1000.0f / (nowMs - fpsWindowStartMs);
        fpsWindowStartMs = nowMs;
        fpsFrameCount = 0;
    }
    delay(0);
}
