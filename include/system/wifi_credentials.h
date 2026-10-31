#pragma once

#include <atomic>
#include <cstdint>

namespace ledlab {

struct WifiNetworkProfile {
    char ssid[33]{};
    char password[64]{};
};

// Three remembered networks, tried in saved order. The newest network is first.
struct WifiCredentials {
    WifiNetworkProfile networks[3]{};
};

enum class WifiConnectionState : uint8_t {
    NotConfigured,
    Connecting,
    Connected,
    Disconnected
};

struct WifiDiagnostics {
    std::atomic<WifiConnectionState> state{WifiConnectionState::NotConfigured};
    std::atomic<uint8_t> disconnectReason{0};
    std::atomic<int16_t> disconnectRssi{0};
};

}  // namespace ledlab
