#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#include "device_config.h"
#include "led/led_output.h"
#include "runtime/animation_runtime.h"
#include "system/device_state.h"
#include "system/wifi_credentials.h"
#include "version.h"
#include "web/web_server.h"

namespace {

ledlab::AnimationRuntime runtime;
ledlab::LedOutput leds;
ledlab::DeviceSettings settings;
ledlab::DeviceMetrics metrics;
ledlab::WifiCredentials wifiCredentials;
ledlab::WifiDiagnostics wifiDiagnostics;
ledlab::WebServerApp web(runtime, leds, settings, metrics, wifiCredentials,
                         wifiDiagnostics);
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
        default:
            break;
    }
}

bool startAccessPoint() {
    const IPAddress ip(192, 168, 4, 1);
    const IPAddress gateway(192, 168, 4, 1);
    const IPAddress subnet(255, 255, 255, 0);
    WiFi.mode(WIFI_AP_STA);
    if (!WiFi.setSleep(false)) {
        logLine("error", "Could not disable Wi-Fi power saving");
    }
    if (!WiFi.setTxPower(WIFI_POWER_19_5dBm)) {
        logLine("error", "Could not set maximum Wi-Fi transmit power");
    }
    if (!WiFi.softAPConfig(ip, gateway, subnet)) {
        logLine("error", "Could not configure access point network");
        return false;
    }
    if (!WiFi.softAP(ledlab::config::WIFI_SSID, ledlab::config::WIFI_PASSWORD)) {
        logLine("error", "Could not start Wi-Fi access point");
        return false;
    }
    Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n",
                  ledlab::config::WIFI_SSID, WiFi.softAPIP().toString().c_str());
    return true;
}

bool loadWifiCredentials() {
    Preferences preferences;
    if (!preferences.begin("ledlab", false)) {
        logLine("error", "Could not open Wi-Fi credential storage");
        return false;
    }

    bool loaded = false;
    if (preferences.getBytesLength("wifi") == sizeof(wifiCredentials) &&
        preferences.getBytes("wifi", &wifiCredentials, sizeof(wifiCredentials)) ==
            sizeof(wifiCredentials)) {
        loaded = wifiCredentials.ssid[0] != '\0' &&
                 std::memchr(wifiCredentials.ssid, '\0',
                             sizeof(wifiCredentials.ssid)) != nullptr &&
                 std::memchr(wifiCredentials.password, '\0',
                             sizeof(wifiCredentials.password)) != nullptr;
    }
    preferences.end();

    if (loaded) {
        wifiDiagnostics.state.store(ledlab::WifiConnectionState::Connecting);
        WiFi.setAutoReconnect(true);
        WiFi.begin(wifiCredentials.ssid, wifiCredentials.password);
        Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"message\":\"Connecting to saved home network\",\"ssid\":\"%s\"}\n",
                      wifiCredentials.ssid);
    }
    return loaded;
}

}  // namespace

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

    startAccessPoint();
    loadWifiCredentials();
    if (!LittleFS.begin(false, "/littlefs", 10, "littlefs")) {
        logLine("error", "LittleFS mount failed; web assets are unavailable");
    } else {
        Serial.printf("{\"level\":\"info\",\"component\":\"filesystem\",\"totalBytes\":%lu,\"usedBytes\":%lu}\n",
                      static_cast<unsigned long>(LittleFS.totalBytes()),
                      static_cast<unsigned long>(LittleFS.usedBytes()));
    }
    runtime.setTimeScale(settings.timeScale);
    if (!web.begin()) {
        logLine("error", "HTTP server initialization failed");
    } else {
        logLine("info", "HTTP server started on port 80");
    }
    logLine("info", "Runtime ready; no animation is running");
}

void loop() {
    std::lock_guard<std::mutex> stateLock(web.stateMutex());
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
    if (nowMs - fpsWindowStartMs >= 1000) {
        metrics.actualFps = fpsFrameCount * 1000.0f / (nowMs - fpsWindowStartMs);
        fpsWindowStartMs = nowMs;
        fpsFrameCount = 0;
    }
    delay(0);
}
