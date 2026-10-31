#pragma once

#include <DNSServer.h>

#include <atomic>
#include <cstdint>

#include "system/device_state.h"
#include "system/wifi_credentials.h"

namespace ledlab {

class WifiManager {
public:
    WifiManager(WifiCredentials& credentials, WifiDiagnostics& diagnostics);

    bool begin();
    void service();

    // Requests are consumed by service() from the main application loop.
    void requestConnect();
    void setScanning(bool scanning);

    bool accessPointActive() const;
    bool mdnsActive() const;
    bool captivePortalActive() const;

private:
    bool hasCredentials() const;
    void configureRadio();
    bool startAccessPoint(bool keepStation);
    void stopAccessPoint();
    void startMdns();
    void stopMdns();
    void startStation(bool useBegin);
    uint32_t nextRetryDelay();

    WifiCredentials& credentials_;
    WifiDiagnostics& diagnostics_;
    uint8_t currentProfileIndex_ = 0;
    DNSServer dnsServer_;

    std::atomic<bool> connectRequested_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<uint32_t> scanStartedAtMs_{0};
    std::atomic<bool> apActive_{false};
    std::atomic<bool> mdnsActive_{false};

    bool dnsActive_ = false;
    bool stationStarted_ = false;
    bool stationWasConnected_ = false;
    bool connectScheduled_ = false;
    uint8_t retryAttempt_ = 0;

    uint32_t connectAtMs_ = 0;
    uint32_t stationAttemptStartedAtMs_ = 0;
    uint32_t nextRetryAtMs_ = 0;
    uint32_t apShutdownAtMs_ = 0;
    uint32_t nextMdnsAttemptAtMs_ = 0;
};

}  // namespace ledlab
