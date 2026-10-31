#include "web/web_server.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <new>
#include <utility>

#include "version.h"

namespace ledlab {
namespace {

constexpr uint16_t STATUS_OK = 200;
constexpr uint16_t STATUS_ACCEPTED = 202;
constexpr uint16_t STATUS_BAD_REQUEST = 400;
constexpr uint16_t STATUS_NOT_FOUND = 404;
constexpr uint16_t STATUS_CONFLICT = 409;
constexpr uint16_t STATUS_TOO_LARGE = 413;
constexpr uint16_t STATUS_INTERNAL_SERVER_ERROR = 500;

struct RequestBodyBuffer {
    String value;
    bool tooLarge = false;
    bool allocationFailed = false;
};

const char* mimeType(const String& path) {
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css")) return "text/css; charset=utf-8";
    if (path.endsWith(".js")) return "application/javascript; charset=utf-8";
    if (path.endsWith(".svg")) return "image/svg+xml";
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

bool isValidUtf8(const String& value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(value.c_str());
    const size_t length = value.length();
    size_t i = 0;
    while (i < length) {
        const uint8_t first = bytes[i++];
        if (first <= 0x7f) continue;

        uint32_t codePoint = 0;
        size_t continuationCount = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codePoint = first & 0x1f;
            continuationCount = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codePoint = first & 0x0f;
            continuationCount = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codePoint = first & 0x07;
            continuationCount = 3;
        } else {
            return false;
        }
        if (i + continuationCount > length) return false;
        for (size_t j = 0; j < continuationCount; ++j) {
            const uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            (codePoint >= 0xd800 && codePoint <= 0xdfff) ||
            codePoint > 0x10ffff) {
            return false;
        }
    }
    return true;
}

String ssidHex(const String& ssid) {
    static const char digits[] = "0123456789abcdef";
    String result;
    result.reserve(ssid.length() * 2);
    for (size_t i = 0; i < ssid.length(); ++i) {
        const uint8_t byte = static_cast<uint8_t>(ssid[i]);
        result += digits[byte >> 4];
        result += digits[byte & 0x0f];
    }
    return result;
}

int hexValue(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

const char* wifiStateName(WifiConnectionState state) {
    switch (state) {
        case WifiConnectionState::NotConfigured: return "not-configured";
        case WifiConnectionState::Connecting: return "connecting";
        case WifiConnectionState::Connected: return "connected";
        case WifiConnectionState::Disconnected: return "disconnected";
    }
    return "unknown";
}

}  // namespace

WebServerApp::WebServerApp(AnimationRuntime& runtime, LedOutput& leds,
                           DeviceSettings& settings, DeviceMetrics& metrics,
                           WifiCredentials& wifiCredentials,
                           WifiDiagnostics& wifiDiagnostics)
    : server_(config::HTTP_PORT), runtime_(runtime), leds_(leds),
      settings_(settings), metrics_(metrics), wifiCredentials_(wifiCredentials),
      wifiDiagnostics_(wifiDiagnostics) {}

bool WebServerApp::begin() {
    registerRoutes();
    server_.begin();
    return true;
}

void WebServerApp::resumeHomeWifi() {
    if (!wifiScanResumeStation_) return;

    wifiScanResumeStation_ = false;
    WiFi.setAutoReconnect(true);
    wifiDiagnostics_.state.store(WifiConnectionState::Connecting);
    WiFi.begin(wifiCredentials_.ssid, wifiCredentials_.password);
    Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"scan_station_resumed\"}\n");
}

bool WebServerApp::takeStepRequest() {
    const bool requested = stepRequested_;
    stepRequested_ = false;
    return requested;
}

bool WebServerApp::takeOutputRefreshRequest() {
    const bool requested = outputRefreshRequested_;
    outputRefreshRequested_ = false;
    return requested;
}

bool WebServerApp::takeClearRequest() {
    const bool requested = clearRequested_;
    clearRequested_ = false;
    return requested;
}

void WebServerApp::sendJson(AsyncWebServerRequest* request, uint16_t status,
                            JsonDocument& document) {
    String payload;
    serializeJson(document, payload);
    Serial.printf("{\"level\":\"info\",\"component\":\"http\",\"event\":\"json_response\",\"status\":%u,\"bytes\":%u}\n",
                  status, static_cast<unsigned>(payload.length()));
    request->send(status, "application/json; charset=utf-8", payload);
}

void WebServerApp::sendError(AsyncWebServerRequest* request, uint16_t status,
                             const char* code, const String& message,
                             uint16_t line, uint16_t column) {
    JsonDocument document;
    document["success"] = false;
    JsonObject error = document["error"].to<JsonObject>();
    error["code"] = code;
    error["message"] = message;
    if (line > 0) error["line"] = line;
    if (column > 0) error["column"] = column;
    sendJson(request, status, document);
}

void WebServerApp::registerJsonPost(const char* path, size_t maxBodySize,
                                    JsonPostHandler handler) {
    server_.on(path, HTTP_POST,
        [this, maxBodySize, handler](
            AsyncWebServerRequest* request) {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            auto* bodyBuffer =
                static_cast<RequestBodyBuffer*>(request->_tempObject);
            request->_tempObject = nullptr;
            if (request->hasAttribute("body_buffer_failed")) {
                delete bodyBuffer;
                sendError(request, STATUS_INTERNAL_SERVER_ERROR,
                          "REQUEST_BUFFER_ERROR",
                          "Could not allocate memory for the request body");
                return;
            }
            if (bodyBuffer && bodyBuffer->allocationFailed) {
                delete bodyBuffer;
                sendError(request, STATUS_INTERNAL_SERVER_ERROR,
                          "REQUEST_BUFFER_ERROR",
                          "Could not buffer the request body");
                return;
            }
            if (bodyBuffer && bodyBuffer->tooLarge) {
                delete bodyBuffer;
                sendError(request, STATUS_TOO_LARGE, "REQUEST_TOO_LARGE",
                          "Request body exceeds the allowed size");
                return;
            }

            String body;
            if (bodyBuffer) {
                body = std::move(bodyBuffer->value);
                delete bodyBuffer;
            }
            handler(request, body);
        },
        nullptr,
        [maxBodySize](AsyncWebServerRequest* request, uint8_t* data,
                      size_t length, size_t index, size_t total) {
            if (index == 0 && !request->_tempObject &&
                !request->hasAttribute("body_buffer_failed")) {
                auto* bodyBuffer = new (std::nothrow) RequestBodyBuffer;
                if (!bodyBuffer) {
                    request->setAttribute("body_buffer_failed", true);
                    return;
                }
                request->_tempObject = bodyBuffer;
                request->onDisconnect([request]() {
                    auto* pendingBody =
                        static_cast<RequestBodyBuffer*>(request->_tempObject);
                    delete pendingBody;
                    request->_tempObject = nullptr;
                });
            }

            auto* bodyBuffer =
                static_cast<RequestBodyBuffer*>(request->_tempObject);
            if (!bodyBuffer || bodyBuffer->tooLarge ||
                bodyBuffer->allocationFailed) {
                return;
            }
            if (total > maxBodySize ||
                index != bodyBuffer->value.length() ||
                length > maxBodySize - bodyBuffer->value.length()) {
                bodyBuffer->tooLarge = true;
                return;
            }
            if (!bodyBuffer->value.concat(
                    reinterpret_cast<const char*>(data), length)) {
                bodyBuffer->allocationFailed = true;
            }
        });
}

void WebServerApp::registerRoutes() {
    server_.on("/api/v1/status", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        Serial.printf("{\"level\":\"info\",\"component\":\"http\",\"event\":\"status_request\",\"home_ip\":\"%s\",\"wifi_status\":%u}\n",
                      WiFi.localIP().toString().c_str(),
                      static_cast<unsigned>(WiFi.status()));
        JsonDocument document;
        document["connected"] = true;
        document["device"] = "ESP32-WROOM-32";

        JsonObject runtime = document["runtime"].to<JsonObject>();
        runtime["running"] = runtime_.running();
        runtime["animationName"] = runtime_.animationName();
        runtime["frame"] = runtime_.frameCount();
        runtime["fps"] = metrics_.actualFps;
        runtime["frameTimeMs"] = metrics_.frameTimeMs;
        runtime["computeTimeMs"] = runtime_.lastComputeTimeUs() / 1000.0f;
        runtime["transferTimeMs"] = metrics_.transferTimeMs;
        runtime["missedFrames"] = metrics_.missedFrames;
        runtime["simulationTime"] = runtime_.simulationTime();
        runtime["timeScale"] = settings_.timeScale;
        runtime["targetFps"] = settings_.targetFps;
        if (!metrics_.lastErrorCode.empty()) {
            JsonObject error = runtime["lastError"].to<JsonObject>();
            error["code"] = metrics_.lastErrorCode;
            error["message"] = metrics_.lastErrorMessage;
            if (metrics_.lastErrorLine) error["line"] = metrics_.lastErrorLine;
            if (metrics_.lastErrorColumn) error["column"] = metrics_.lastErrorColumn;
        }

        JsonObject led = document["led"].to<JsonObject>();
        led["count"] = config::LED_COUNT;
        led["type"] = "WS2812B";
        led["brightness"] = settings_.brightness;
        led["effectiveBrightness"] = leds_.effectiveBrightness();
        led["currentLimitMa"] = leds_.currentLimitMa();
        led["estimatedCurrentMa"] = leds_.estimatedCurrentMa();
        led["powerLimited"] = leds_.powerLimited();
        document["settings"]["brightness"] = settings_.brightness;
        document["settings"]["currentLimitMa"] = settings_.currentLimitMa;
        document["settings"]["timeScale"] = settings_.timeScale;
        document["settings"]["targetFps"] = settings_.targetFps;

        JsonObject wifi = document["wifi"].to<JsonObject>();
        wifi["mode"] = "AP+STA";
        wifi["ssid"] = WiFi.softAPSSID();
        wifi["ip"] = WiFi.softAPIP().toString();
        JsonObject homeWifi = wifi["home"].to<JsonObject>();
        homeWifi["configured"] = wifiCredentials_.ssid[0] != '\0';
        const String savedSsid(wifiCredentials_.ssid);
        const String savedSsidBytes = ssidHex(savedSsid);
        homeWifi["ssid"] = isValidUtf8(savedSsid)
            ? savedSsid : String("Non-UTF-8 SSID [") + savedSsidBytes + "]";
        homeWifi["ssidHex"] = savedSsidBytes;
        const WifiConnectionState state = wifiDiagnostics_.state.load();
        homeWifi["connected"] = state == WifiConnectionState::Connected &&
                                WiFi.status() == WL_CONNECTED;
        homeWifi["status"] = wifiStateName(state);
        const uint8_t disconnectReason = wifiDiagnostics_.disconnectReason.load();
        homeWifi["disconnectReason"] = disconnectReason;
        homeWifi["disconnectRssi"] = wifiDiagnostics_.disconnectRssi.load();
        homeWifi["disconnectReasonName"] = disconnectReason == 0
            ? "" : WiFi.disconnectReasonName(
                static_cast<wifi_err_reason_t>(disconnectReason));
        homeWifi["ip"] = WiFi.localIP().toString();
        sendJson(request, STATUS_OK, document);
    });

    server_.on("/api/v1/capabilities", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        JsonDocument document;
        document["firmwareVersion"] = LEDLAB_FIRMWARE_VERSION;
        document["apiVersion"] = LEDLAB_API_VERSION;
        document["dslVersion"] = LEDLAB_DSL_VERSION;
        document["ledCount"] = config::LED_COUNT;
        document["ledType"] = "WS2812B";
        document["maxSourceBytes"] = config::MAX_SOURCE_BYTES;
        document["maxArrayLength"] = config::MAX_ARRAY_LENGTH;
        document["maxLoopIterations"] = config::MAX_LOOP_ITERATIONS;
        JsonArray rates = document["supportedOutputRates"].to<JsonArray>();
        rates.add(20);
        rates.add(30);
        rates.add(60);
        JsonObject settings = document["settings"].to<JsonObject>();
        settings["brightnessMin"] = 0;
        settings["brightnessMax"] = 100;
        settings["currentLimitMinMa"] = 100;
        settings["currentLimitMaxMa"] = config::CURRENT_LIMIT_MA_MAX;
        settings["timeScaleMin"] = 0.1;
        settings["timeScaleMax"] = 4.0;
        document["filesystemTotalBytes"] = LittleFS.totalBytes();
        document["filesystemUsedBytes"] = LittleFS.usedBytes();
        sendJson(request, STATUS_OK, document);
    });

    server_.on("/api/v1/wifi/scan", HTTP_POST,
               [this](AsyncWebServerRequest* request) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (wifiScanActive_ && WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
            JsonDocument response;
            response["status"] = "scanning";
            sendJson(request, STATUS_ACCEPTED, response);
            return;
        }
        wifiScanActive_ = false;
        WiFi.scanDelete();
        wifiScanResumeStation_ = wifiCredentials_.ssid[0] != '\0';
        if (wifiScanResumeStation_) {
            WiFi.setAutoReconnect(false);
            if (!WiFi.disconnect(false, false)) {
                resumeHomeWifi();
                sendError(request, STATUS_INTERNAL_SERVER_ERROR, "WIFI_SCAN_FAILED",
                          "ESP32 could not pause its home Wi-Fi connection for scanning");
                return;
            }
            delay(150);
        }
        Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"scan_started\"}\n");
        if (WiFi.scanNetworks(true, true, false, 120) == WIFI_SCAN_FAILED) {
            Serial.printf("{\"level\":\"error\",\"component\":\"wifi\",\"event\":\"scan_failed\"}\n");
            resumeHomeWifi();
            sendError(request, STATUS_INTERNAL_SERVER_ERROR, "WIFI_SCAN_FAILED",
                      "ESP32 could not start a Wi-Fi scan");
            return;
        }
        wifiScanActive_ = true;
        JsonDocument response;
        response["status"] = "scanning";
        sendJson(request, STATUS_ACCEPTED, response);
    });

    server_.on("/api/v1/wifi/scan", HTTP_GET,
               [this](AsyncWebServerRequest* request) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        JsonDocument response;
        if (!wifiScanActive_) {
            response["status"] = "idle";
            sendJson(request, STATUS_OK, response);
            return;
        }

        const int16_t result = WiFi.scanComplete();
        if (result == WIFI_SCAN_RUNNING) {
            response["status"] = "scanning";
            sendJson(request, STATUS_OK, response);
            return;
        }
        if (result == WIFI_SCAN_FAILED) {
            wifiScanActive_ = false;
            WiFi.scanDelete();
            Serial.printf("{\"level\":\"error\",\"component\":\"wifi\",\"event\":\"scan_failed\"}\n");
            resumeHomeWifi();
            sendError(request, STATUS_INTERNAL_SERVER_ERROR, "WIFI_SCAN_FAILED",
                      "Wi-Fi scan failed; try again");
            return;
        }

        wifiScanActive_ = false;
        response["status"] = "complete";
        JsonArray networks = response["networks"].to<JsonArray>();
        for (int16_t i = 0; i < result; ++i) {
            const String ssid = WiFi.SSID(static_cast<uint8_t>(i));
            if (ssid.isEmpty()) continue;
            JsonObject network = networks.add<JsonObject>();
            const String encodedSsid = ssidHex(ssid);
            const wifi_auth_mode_t authMode =
                WiFi.encryptionType(static_cast<uint8_t>(i));
            network["ssid"] = isValidUtf8(ssid)
                ? ssid : String("Non-UTF-8 SSID [") + encodedSsid + "]";
            network["ssidHex"] = encodedSsid;
            network["rssi"] = WiFi.RSSI(static_cast<uint8_t>(i));
            network["channel"] = WiFi.channel(static_cast<uint8_t>(i));
            network["authMode"] = static_cast<uint8_t>(authMode);
            network["secure"] = authMode != WIFI_AUTH_OPEN;
        }
        Serial.printf("{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"scan_complete\",\"networks\":%u}\n",
                      static_cast<unsigned>(networks.size()));
        WiFi.scanDelete();
        resumeHomeWifi();
        sendJson(request, STATUS_OK, response);
    });

    registerJsonPost("/api/v1/wifi", 256,
                     [this](AsyncWebServerRequest* request,
                            const String& body) {
        JsonDocument document;
        const DeserializationError parseError = deserializeJson(document, body);
        if (parseError) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_JSON", "Malformed JSON request body");
            return;
        }
        const bool hasSsid = document["ssid"].is<const char*>();
        const bool hasSsidHex = document["ssidHex"].is<const char*>();
        if (!document.is<JsonObject>() || hasSsid == hasSsidHex ||
            !document["password"].is<const char*>()) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                      "Wi-Fi settings require exactly one string ssid or ssidHex field and a string password");
            return;
        }

        const String password = document["password"].as<String>();
        WifiCredentials candidate{};
        if (hasSsidHex) {
            const String encodedSsid = document["ssidHex"].as<String>();
            if (encodedSsid.isEmpty() || encodedSsid.length() > 64 ||
                (encodedSsid.length() & 1U) != 0U) {
                sendError(request, STATUS_BAD_REQUEST, "INVALID_SSID",
                          "Encoded SSID must contain 1 to 32 bytes");
                return;
            }
            const size_t decodedLength = encodedSsid.length() / 2;
            for (size_t i = 0; i < decodedLength; ++i) {
                const int high = hexValue(encodedSsid[i * 2]);
                const int low = hexValue(encodedSsid[i * 2 + 1]);
                if (high < 0 || low < 0 || (high == 0 && low == 0)) {
                    sendError(request, STATUS_BAD_REQUEST, "INVALID_SSID",
                              "Encoded SSID contains invalid bytes");
                    return;
                }
                candidate.ssid[i] = static_cast<char>((high << 4) | low);
            }
        } else {
            const String ssid = document["ssid"].as<String>();
            if (ssid.isEmpty() || ssid.length() > sizeof(candidate.ssid) - 1) {
                sendError(request, STATUS_BAD_REQUEST, "INVALID_SSID", "SSID must be 1 to 32 bytes");
                return;
            }
            ssid.toCharArray(candidate.ssid, sizeof(candidate.ssid));
        }
        if (password.length() > sizeof(WifiCredentials::password) - 1 ||
            (!password.isEmpty() && password.length() < 8)) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_PASSWORD",
                      "Password must be empty for an open network or 8 to 63 bytes");
            return;
        }

        password.toCharArray(candidate.password, sizeof(candidate.password));

        Preferences preferences;
        if (!preferences.begin("ledlab", false)) {
            sendError(request, STATUS_INTERNAL_SERVER_ERROR, "STORAGE_ERROR",
                      "Could not open Wi-Fi credential storage");
            return;
        }
        const size_t written = preferences.putBytes("wifi", &candidate, sizeof(candidate));
        preferences.end();
        if (written != sizeof(candidate)) {
            sendError(request, STATUS_INTERNAL_SERVER_ERROR, "STORAGE_ERROR",
                      "Could not save Wi-Fi credentials");
            return;
        }

        wifiCredentials_ = candidate;
        wifiDiagnostics_.state.store(WifiConnectionState::Connecting);
        wifiDiagnostics_.disconnectReason.store(0);
        wifiDiagnostics_.disconnectRssi.store(0);
        JsonDocument response;
        response["success"] = true;
        response["status"] = "connecting";
        const String savedSsid(wifiCredentials_.ssid);
        response["ssid"] = isValidUtf8(savedSsid)
            ? savedSsid : String("Non-UTF-8 SSID [") + ssidHex(savedSsid) + "]";
        sendJson(request, STATUS_ACCEPTED, response);

        WiFi.setAutoReconnect(true);
        WiFi.disconnect(false, false);
        WiFi.begin(wifiCredentials_.ssid, wifiCredentials_.password);
    });

    registerJsonPost("/api/v1/animation/compile-run",
                     config::MAX_SOURCE_BYTES + 512,
                     [this](AsyncWebServerRequest* request,
                            const String& body) {
        if (runtime_.job().pending) {
            sendError(request, STATUS_CONFLICT, "JOB_RUNNING", "An animation compilation is already queued");
            return;
        }
        JsonDocument document;
        const DeserializationError parseError = deserializeJson(document, body);
        if (parseError) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_JSON", "Malformed JSON request body");
            return;
        }
        if (!document.is<JsonObject>()) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "Request body must be a JSON object");
            return;
        }
        const JsonVariantConst sourceValue = document["source"];
        if (!sourceValue.is<const char*>()) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "The source field is required");
            return;
        }
        const char* sourceText = sourceValue.as<const char*>();
        const String source(sourceText);
        if (source.length() > config::MAX_SOURCE_BYTES) {
            sendError(request, STATUS_TOO_LARGE, "SOURCE_TOO_LARGE", "Animation source exceeds 16384 bytes");
            return;
        }
        const String name = document["name"].is<const char*>()
                                ? document["name"].as<String>()
                                : String();
        if (!document["name"].isNull() && !document["name"].is<const char*>()) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "The name field must be a string");
            return;
        }
        RuntimeError error;
        uint32_t jobId = 0;
        if (!runtime_.queueCompile(name.c_str(), source.c_str(), jobId, error)) {
            sendError(request, STATUS_BAD_REQUEST, error.code.c_str(), error.message.c_str(),
                      error.line, error.column);
            return;
        }
        JsonDocument response;
        response["accepted"] = true;
        response["jobId"] = jobId;
        response["status"] = "queued";
        sendJson(request, STATUS_ACCEPTED, response);
    });

    registerJsonPost("/api/v1/animation/stop", 256,
                     [this](AsyncWebServerRequest* request,
                            const String& body) {
        bool clear = false;
        if (!body.isEmpty()) {
            JsonDocument document;
            if (deserializeJson(document, body)) {
                sendError(request, STATUS_BAD_REQUEST, "INVALID_JSON", "Malformed JSON request body");
                return;
            }
            if (!document.is<JsonObject>() ||
                (!document["clear"].isNull() && !document["clear"].is<bool>())) {
                sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "Stop request must contain an optional boolean clear field");
                return;
            }
            clear = document["clear"] | false;
        }
        runtime_.stop();
        if (clear) clearRequested_ = true;
        JsonDocument response;
        response["success"] = true;
        response["running"] = false;
        response["cleared"] = clear;
        sendJson(request, STATUS_OK, response);
    });

    server_.on("/api/v1/animation/step", HTTP_POST,
               [this](AsyncWebServerRequest* request) {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (runtime_.running()) {
            sendError(request, STATUS_CONFLICT, "ANIMATION_RUNNING", "Stop the animation before stepping");
            return;
        }
        if (runtime_.animationName().empty()) {
            sendError(request, STATUS_CONFLICT, "NO_ANIMATION", "Compile an animation before stepping");
            return;
        }
        stepRequested_ = true;
        JsonDocument response;
        response["accepted"] = true;
        response["status"] = "queued";
        sendJson(request, STATUS_ACCEPTED, response);
    });

    registerJsonPost("/api/v1/settings", 512,
                     [this](AsyncWebServerRequest* request,
                            const String& body) {
        JsonDocument document;
        const DeserializationError parseError = deserializeJson(document, body);
        if (parseError) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_JSON", "Malformed JSON request body");
            return;
        }
        if (!document.is<JsonObject>()) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "Settings body must be a JSON object");
            return;
        }
        if ((!document["brightness"].isNull() && !document["brightness"].is<int>()) ||
            (!document["currentLimitMa"].isNull() && !document["currentLimitMa"].is<int>()) ||
            (!document["targetFps"].isNull() && !document["targetFps"].is<int>()) ||
            (!document["timeScale"].isNull() && !document["timeScale"].is<float>() &&
             !document["timeScale"].is<double>())) {
            sendError(request, STATUS_BAD_REQUEST, "INVALID_SETTING", "One or more settings have an invalid type");
            return;
        }
        DeviceSettings effective = settings_;
        if (document["brightness"].is<int>()) {
            effective.brightness = static_cast<uint8_t>(std::max(0, std::min(100,
                document["brightness"].as<int>())));
        }
        if (document["currentLimitMa"].is<int>()) {
            effective.currentLimitMa = static_cast<uint16_t>(std::max<int>(config::CURRENT_LIMIT_MA_MIN, std::min<int>(
                config::CURRENT_LIMIT_MA_MAX, document["currentLimitMa"].as<int>())));
        }
        if (document["targetFps"].is<int>()) {
            const int fps = document["targetFps"].as<int>();
            effective.targetFps = static_cast<uint16_t>(
                fps <= 20 ? 20 : fps <= 30 ? 30 : 60);
        }
        if (document["timeScale"].is<float>()) {
            const float scale = document["timeScale"].as<float>();
            if (!std::isfinite(scale)) {
                sendError(request, STATUS_BAD_REQUEST, "INVALID_SETTING", "timeScale must be finite");
                return;
            }
            effective.timeScale = std::max(0.1f, std::min(4.0f, scale));
        }
        settings_ = effective;
        runtime_.setTimeScale(settings_.timeScale);
        outputRefreshRequested_ = true;
        JsonDocument response;
        response["success"] = true;
        JsonObject serializedSettings = response["settings"].to<JsonObject>();
        serializedSettings["brightness"] = settings_.brightness;
        serializedSettings["currentLimitMa"] = settings_.currentLimitMa;
        serializedSettings["targetFps"] = settings_.targetFps;
        serializedSettings["timeScale"] = settings_.timeScale;
        sendJson(request, STATUS_OK, response);
    });

    server_.onNotFound([this](AsyncWebServerRequest* request) {
        const String path = request->url();
        if (!path.startsWith("/api/v1/")) {
            serveStatic(request);
            return;
        }

        std::lock_guard<std::mutex> stateLock(stateMutex_);
        if (path.startsWith("/api/v1/jobs/")) {
            const uint32_t id = strtoul(path.substring(strlen("/api/v1/jobs/")).c_str(), nullptr, 10);
            const RuntimeJob& job = runtime_.job();
            if (id == 0 || id != job.id) {
                sendError(request, STATUS_NOT_FOUND, "JOB_NOT_FOUND", "Compilation job not found");
                return;
            }
            JsonDocument response;
            response["jobId"] = job.id;
            response["status"] = job.pending ? "queued" : job.complete
                ? (job.success ? "succeeded" : "failed") : "queued";
            if (job.complete && job.success) response["animationName"] = job.name;
            if (job.complete && !job.success) {
                JsonObject error = response["error"].to<JsonObject>();
                error["code"] = job.error.code;
                error["message"] = job.error.message;
                if (job.error.line) error["line"] = job.error.line;
                if (job.error.column) error["column"] = job.error.column;
            }
            sendJson(request, STATUS_OK, response);
            return;
        }
        sendError(request, STATUS_NOT_FOUND, "NOT_FOUND", "Unknown API route");
    });
}

void WebServerApp::serveStatic(AsyncWebServerRequest* request) {
    Serial.printf("{\"level\":\"info\",\"component\":\"http\",\"event\":\"static_request\",\"uri\":\"%s\"}\n",
                  request->url().c_str());
    if (request->method() != HTTP_GET) {
        sendError(request, STATUS_NOT_FOUND, "NOT_FOUND", "Resource not found");
        return;
    }
    String path = request->url();
    if (path == "/") path = "/index.html";
    if (path.indexOf("..") >= 0 || path.indexOf('\\') >= 0) {
        sendError(request, STATUS_BAD_REQUEST, "INVALID_PATH", "Invalid resource path");
        return;
    }
    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) {
        sendError(request, STATUS_NOT_FOUND, "NOT_FOUND", "Resource not found");
        return;
    }
    const size_t fileSize = file.size();
    file.close();
    AsyncWebServerResponse* response =
        request->beginResponse(LittleFS, path, mimeType(path));
    if (!response) {
        sendError(request, STATUS_INTERNAL_SERVER_ERROR, "RESPONSE_ERROR",
                  "Could not create the file response");
        return;
    }
    response->addHeader("Cache-Control", path == "/index.html"
        ? "no-cache" : "public, max-age=3600");
    Serial.printf("{\"level\":\"info\",\"component\":\"http\",\"event\":\"static_response_started\",\"uri\":\"%s\",\"bytes\":%u}\n",
                  path.c_str(), static_cast<unsigned>(fileSize));
    request->send(response);
}

}  // namespace ledlab
