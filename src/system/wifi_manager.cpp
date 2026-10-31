#include "system/wifi_manager.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "device_config.h"

namespace ledlab {
namespace {

constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t CONNECT_GRACE_MS = 1200;
constexpr uint32_t INITIAL_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t AP_GRACE_AFTER_CONNECT_MS = 12000;
constexpr uint32_t MDNS_RETRY_MS = 5000;
constexpr uint32_t MAX_RETRY_MS = 30000;

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);

bool deadlineReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void logWifi(const char* level, const char* event, const char* message = nullptr) {
    Serial.printf(
        "{\"level\":\"%s\",\"component\":\"wifi\",\"event\":\"%s\"",
        level, event);
    if (message) {
        Serial.printf(",\"message\":\"%s\"", message);
    }
    Serial.println("}");
    Serial.flush();
}

}  // namespace

WifiManager::WifiManager(WifiCredentials& credentials, WifiDiagnostics& diagnostics)
    : credentials_(credentials), diagnostics_(diagnostics) {}

bool WifiManager::hasCredentials() const {
    for (const auto& profile : credentials_.networks) {
        if (profile.ssid[0] != '\0') return true;
    }
    return false;
}

void WifiManager::configureRadio() {
    logWifi("info", "radio_sleep_setting_begin");
    const bool sleepConfigured = WiFi.setSleep(false);
    Serial.printf(
        "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"radio_sleep_setting_complete\",\"success\":%s}\n",
        sleepConfigured ? "true" : "false");
    Serial.flush();
    if (!sleepConfigured) {
        logWifi("warning", "sleep_setting_failed", "Could not disable Wi-Fi power saving");
    }

    logWifi("info", "radio_tx_power_setting_begin");
    const bool txPowerConfigured = WiFi.setTxPower(WIFI_POWER_19_5dBm);
    Serial.printf(
        "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"radio_tx_power_setting_complete\",\"success\":%s}\n",
        txPowerConfigured ? "true" : "false");
    Serial.flush();
    if (!txPowerConfigured) {
        logWifi("warning", "tx_power_setting_failed", "Could not set requested TX power");
    }

    logWifi("info", "radio_auto_reconnect_setting_begin");
    WiFi.setAutoReconnect(false);
    logWifi("info", "radio_auto_reconnect_setting_complete");
}

bool WifiManager::begin() {
    logWifi("info", "manager_begin");

    // The DHCP hostname must be set before Wi-Fi mode or interfaces are started.
    logWifi("info", "hostname_setting_begin");
    const bool hostnameSet = WiFi.setHostname(config::MDNS_HOSTNAME);
    Serial.printf(
        "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"hostname_setting_complete\",\"success\":%s}\n",
        hostnameSet ? "true" : "false");
    Serial.flush();
    if (!hostnameSet) {
        logWifi("error", "hostname_failed", "Could not set DHCP hostname");
    }

    // Reconnects are scheduled here, not delegated to an unbounded driver retry loop.
    if (hasCredentials()) {
        logWifi("info", "station_mode_setting_begin");
        const bool stationModeSet = WiFi.mode(WIFI_STA);
        Serial.printf(
            "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"station_mode_setting_complete\",\"success\":%s}\n",
            stationModeSet ? "true" : "false");
        Serial.flush();

        configureRadio();
        diagnostics_.state.store(WifiConnectionState::Connecting);
        currentProfileIndex_ = 0;
        logWifi("info", "station_begin_call");
        WiFi.begin(credentials_.networks[currentProfileIndex_].ssid,
                   credentials_.networks[currentProfileIndex_].password);
        logWifi("info", "station_begin_returned");
        stationStarted_ = true;
        stationAttemptStartedAtMs_ = millis();
        nextRetryAtMs_ = stationAttemptStartedAtMs_ + 1500;
        logWifi("info", "station_connect_started", "Trying saved home Wi-Fi credentials");
        return true;
    }

    diagnostics_.state.store(WifiConnectionState::NotConfigured);
    logWifi("info", "provisioning_required", "No saved home Wi-Fi credentials");
    return startAccessPoint(true);
}

void WifiManager::requestConnect() {
    connectRequested_.store(true, std::memory_order_release);
}

void WifiManager::setScanning(bool scanning) {
    if (scanning) {
        scanStartedAtMs_.store(millis(), std::memory_order_release);
    } else {
        scanStartedAtMs_.store(0, std::memory_order_release);
    }
    scanning_.store(scanning, std::memory_order_release);
}

bool WifiManager::accessPointActive() const {
    return apActive_.load(std::memory_order_acquire);
}

bool WifiManager::mdnsActive() const {
    return mdnsActive_.load(std::memory_order_acquire);
}

bool WifiManager::captivePortalActive() const {
    return accessPointActive() && WiFi.status() != WL_CONNECTED;
}

bool WifiManager::startAccessPoint(bool keepStation) {
    if (apActive_.load(std::memory_order_acquire)) return true;

    const wifi_mode_t mode = keepStation ? WIFI_AP_STA : WIFI_AP;
    if (!WiFi.mode(mode)) {
        logWifi("error", "ap_mode_failed", "Could not enable AP interface");
        return false;
    }
    configureRadio();
    if (!WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET)) {
        logWifi("error", "ap_config_failed", "Could not configure setup AP address");
        return false;
    }
    if (!WiFi.softAP(config::WIFI_SSID, config::WIFI_PASSWORD)) {
        logWifi("error", "ap_start_failed", "Could not start setup access point");
        return false;
    }

    apActive_.store(true, std::memory_order_release);
    dnsActive_ = dnsServer_.start(DNS_PORT, "*", AP_IP);
    if (dnsActive_) {
        logWifi("info", "captive_dns_started", "All DNS names resolve to the setup AP");
    } else {
        logWifi("error", "captive_dns_failed", "Setup AP is running without captive DNS");
    }
    Serial.printf(
        "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"ap_started\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n",
        config::WIFI_SSID, WiFi.softAPIP().toString().c_str());
    return true;
}

void WifiManager::stopAccessPoint() {
    if (!apActive_.load(std::memory_order_acquire)) return;

    // Rebind mDNS after removing the AP interface. The STA address remains authoritative.
    const bool restartMdns = mdnsActive_.load(std::memory_order_acquire);
    if (restartMdns) stopMdns();

    if (dnsActive_) {
        dnsServer_.stop();
        dnsActive_ = false;
    }

    // false is essential: do not switch the radio off while STA is connected.
    WiFi.softAPdisconnect(false);
    WiFi.mode(WIFI_STA);
    apActive_.store(false, std::memory_order_release);
    apShutdownAtMs_ = 0;

    if (restartMdns) {
        if (WiFi.status() == WL_CONNECTED) {
            startMdns();
        } else {
            nextMdnsAttemptAtMs_ = millis() + MDNS_RETRY_MS;
        }
    }
    logWifi("info", "ap_stopped", "STA is connected; setup AP fallback is no longer needed");
}

void WifiManager::startMdns() {
    if (mdnsActive_.load(std::memory_order_acquire) ||
        WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!MDNS.begin(config::MDNS_HOSTNAME)) {
        nextMdnsAttemptAtMs_ = millis() + MDNS_RETRY_MS;
        logWifi("warning", "mdns_start_failed", "Will retry mDNS registration");
        return;
    }

    MDNS.addService("http", "tcp", config::HTTP_PORT);
    mdnsActive_.store(true, std::memory_order_release);
    nextMdnsAttemptAtMs_ = 0;
    Serial.printf(
        "{\"level\":\"info\",\"component\":\"wifi\",\"event\":\"mdns_started\",\"hostname\":\"%s.local\",\"ip\":\"%s\"}\n",
        config::MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
}

void WifiManager::stopMdns() {
    if (!mdnsActive_.exchange(false, std::memory_order_acq_rel)) return;
    MDNS.end();
    logWifi("info", "mdns_stopped");
}

uint32_t WifiManager::nextRetryDelay() {
    const uint8_t exponent = retryAttempt_ < 5 ? retryAttempt_ : 5;
    const uint32_t delayMs = 1000UL << exponent;
    if (retryAttempt_ < 10) ++retryAttempt_;
    return delayMs > MAX_RETRY_MS ? MAX_RETRY_MS : delayMs;
}

void WifiManager::startStation(bool useBegin) {
    if (!hasCredentials()) return;

    // Give each remembered network several attempts before falling back to the next.
    if (!stationWasConnected_ && !useBegin && retryAttempt_ >= 3) {
        uint8_t nextIndex = currentProfileIndex_;
        for (uint8_t offset = 1; offset <= 3; ++offset) {
            const uint8_t candidate = static_cast<uint8_t>(
                (currentProfileIndex_ + offset) % 3);
            if (credentials_.networks[candidate].ssid[0] != '\0') {
                nextIndex = candidate;
                break;
            }
        }
        if (nextIndex != currentProfileIndex_) {
            currentProfileIndex_ = nextIndex;
            retryAttempt_ = 0;
            useBegin = true;
            logWifi("warning", "profile_failover",
                    "Current network failed repeatedly; trying another saved profile");
        }
    }

    const WifiNetworkProfile& active = credentials_.networks[currentProfileIndex_];
    if (active.ssid[0] == '\0') {
        currentProfileIndex_ = 0;
    }

    WiFi.mode(apActive_.load(std::memory_order_acquire) ? WIFI_AP_STA : WIFI_STA);
    configureRadio();

    const WifiNetworkProfile& selected = credentials_.networks[currentProfileIndex_];
    diagnostics_.state.store(WifiConnectionState::Connecting);
    if (useBegin || !stationStarted_) {
        WiFi.begin(selected.ssid, selected.password);
    } else if (!WiFi.reconnect()) {
        WiFi.begin(selected.ssid, selected.password);
    }

    stationStarted_ = true;
    stationAttemptStartedAtMs_ = millis();
    nextRetryAtMs_ = stationAttemptStartedAtMs_ + nextRetryDelay();
    logWifi("info", "station_retry_started", "Trying a saved home Wi-Fi profile");
}

void WifiManager::service() {
    const uint32_t now = millis();

    if (dnsActive_) {
        dnsServer_.processNextRequest();
    }

    if (connectRequested_.exchange(false, std::memory_order_acq_rel)) {
        connectScheduled_ = true;
        connectAtMs_ = now + CONNECT_GRACE_MS;
    }

    if (scanning_.load(std::memory_order_acquire)) {
        const int16_t scanState = WiFi.scanComplete();
        const uint32_t scanStartedAt = scanStartedAtMs_.load(std::memory_order_acquire);
        const bool scanTimedOut = scanStartedAt != 0 &&
                                  now - scanStartedAt >= 20000;
        if (scanState == WIFI_SCAN_RUNNING && !scanTimedOut) {
            return;
        }
        if (scanState == WIFI_SCAN_RUNNING) {
            WiFi.scanDelete();
            logWifi("warning", "scan_timeout", "Wi-Fi scan timed out");
        }
        scanning_.store(false, std::memory_order_release);
        scanStartedAtMs_.store(0, std::memory_order_release);
    }

    if (connectScheduled_ && deadlineReached(now, connectAtMs_)) {
        connectScheduled_ = false;
        stopMdns();
        if (!apActive_.load(std::memory_order_acquire)) {
            startAccessPoint(true);
        } else {
            WiFi.mode(WIFI_AP_STA);
            configureRadio();
        }
        WiFi.disconnect(false, false);
        currentProfileIndex_ = 0;
        diagnostics_.state.store(WifiConnectionState::Connecting);
        WiFi.begin(credentials_.networks[0].ssid,
                   credentials_.networks[0].password);
        stationStarted_ = true;
        stationWasConnected_ = false;
        retryAttempt_ = 0;
        stationAttemptStartedAtMs_ = now;
        nextRetryAtMs_ = now + 1500;
        apShutdownAtMs_ = 0;
        logWifi("info", "credentials_applied", "Connecting with newly saved credentials");
    }

    if (connectScheduled_) {
        return;
    }

    const WifiConnectionState state = diagnostics_.state.load();
    const bool connected = state == WifiConnectionState::Connected &&
                           WiFi.status() == WL_CONNECTED;

    if (connected) {
        if (!stationWasConnected_) {
            stationWasConnected_ = true;
            retryAttempt_ = 0;
            startMdns();
            if (apActive_.load(std::memory_order_acquire)) {
                apShutdownAtMs_ = now + AP_GRACE_AFTER_CONNECT_MS;
            }
            logWifi("info", "station_ready", "Home Wi-Fi is ready");
        } else if (!mdnsActive_.load(std::memory_order_acquire) &&
                   nextMdnsAttemptAtMs_ != 0 &&
                   deadlineReached(now, nextMdnsAttemptAtMs_)) {
            startMdns();
        }

        if (apActive_.load(std::memory_order_acquire) &&
            apShutdownAtMs_ != 0 && deadlineReached(now, apShutdownAtMs_)) {
            stopAccessPoint();
        }
        return;
    }

    if (stationWasConnected_) {
        stationWasConnected_ = false;
        stopMdns();
        startAccessPoint(true);
        apShutdownAtMs_ = 0;
        retryAttempt_ = 0;
        nextRetryAtMs_ = now + 1000;
        logWifi("warning", "station_lost", "Home Wi-Fi lost; setup AP fallback enabled");
    }

    if (!hasCredentials()) {
        if (!apActive_.load(std::memory_order_acquire)) {
            startAccessPoint(true);
        }
        diagnostics_.state.store(WifiConnectionState::NotConfigured);
        return;
    }

    if (!apActive_.load(std::memory_order_acquire) &&
        now - stationAttemptStartedAtMs_ >= INITIAL_CONNECT_TIMEOUT_MS) {
        startAccessPoint(true);
        logWifi("warning", "connection_timeout", "Could not connect; setup AP fallback enabled");
    }

    // WL_DISCONNECTED is also reported while a fresh association is in progress.
    // Use the explicit disconnect event; otherwise retries can interrupt handshakes.
    const bool disconnected = state == WifiConnectionState::Disconnected;
    if (stationStarted_ && disconnected && deadlineReached(now, nextRetryAtMs_)) {
        startStation(false);
    } else if (stationStarted_ &&
               now - stationAttemptStartedAtMs_ >= INITIAL_CONNECT_TIMEOUT_MS &&
               deadlineReached(now, nextRetryAtMs_)) {
        startStation(false);
    }
}

}  // namespace ledlab
