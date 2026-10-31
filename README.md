# LED LAB

LED LAB is a standalone ESP32 access point and WS2812B animation controller. The ESP32 hosts the interface and executes animation DSL programs locally; animation libraries remain in the phone browser's `localStorage`.

## Hardware and wiring

- ESP32-WROOM-32 / ESP32 Dev Module (`esp32dev`)
- WS2812B strip, configured for 300 pixels
- GPIO 23 (D23) data output (change `LED_DATA_PIN` in `include/device_config.h` for a verified alternate pin)
- Separate, regulated 5 V supply for the strip
- Common ground between ESP32 and LED supply
- Recommended: 74AHCT125 level shifter, 330–470 ohm series resistor near the strip data input, and a suitable bulk capacitor across 5 V/GND

Do not power the 300-pixel strip from the ESP32 board or USB. At the conservative 60 mA/pixel estimate, full white can require 18 A (90 W). Size wiring, fusing, power injection, and supply for the actual strip. The firmware starts with black output, defaults to 20% brightness, and estimates output current with a configurable 2 A budget. Its model estimates 1 mA idle per pixel plus 20 mA for each full-scale color channel, weighted by the logical color and requested brightness. If the estimate exceeds the configured budget, the physical frame is scaled to fit the budget after the idle estimate. This model is not a current sensor and does not replace a correctly sized supply or electrical protection.

The setup access point is `LED-LAB` with password `ledlab123`; change `WIFI_PASSWORD` in `include/device_config.h` before deployment. The password must remain at least 8 characters. The setup AP uses `192.168.4.1` and a captive DNS server that resolves hostnames to the setup page. On first boot (or when no credentials are saved), connect to `LED-LAB` and open `http://192.168.4.1`; the captive-portal prompt may also open automatically. The ESP32 connects to home 2.4 GHz Wi-Fi in STA mode. Use **Scan** in the **Home Wi-Fi** panel and select the router SSID; scanning is asynchronous and does not deliberately disconnect the station. Scanned SSIDs are submitted as their original bytes so UTF-8 and other non-ASCII names are not corrupted by browser/JSON encoding. Hidden networks can be entered manually. Credentials are stored in NVS and are not returned by the status API. Up to three distinct networks are remembered: the newest is the primary profile and the previous two remain as backups. The main-loop Wi-Fi manager tries the saved profiles in order after repeated failures. Existing single-network NVS credentials are migrated automatically. New credentials are applied by the main-loop Wi-Fi manager after the HTTP response has been queued, so the setup page can report the transition. After a successful DHCP connection, the firmware registers `ledal.local` with mDNS and advertises HTTP on port 80. The setup AP remains available for a 12-second handoff window, then turns off. Connect the phone or computer to the same home network and open `http://ledal.local`; the DHCP IP is also shown as a fallback for clients without mDNS support. When a saved network cannot be reached on boot, the AP fallback starts after 15 seconds. If a previously working STA connection is lost, the AP fallback returns and STA retries use exponential backoff up to 30 seconds. The HTTP server and its route table are started once and remain independent of AP/STA transitions.

## Build and install

Install PlatformIO Core and the VS Code PlatformIO extension. This project targets PlatformIO Espressif32 8.0.0, Arduino ESP32 Core 3.3.12, FastLED 3.9.15, and ArduinoJson 7.4.2.

```powershell
pio run
pio run -t upload --upload-port COM10
pio device monitor -p COM10 -b 115200

# Optional: upload LittleFS assets that are not embedded in the firmware
pio run -t uploadfs --upload-port COM10
```

Replace `COM10` with the port shown by your computer; no serial port is hardcoded in the project. A PlatformIO pre-build script converts `data/index.html` and `data/examples.json` into PROGMEM assets, so the main app, Wi-Fi setup wizard, and example catalogue are served from the firmware image and do not require `uploadfs`. Upload LittleFS only when you add separate runtime assets there. `partitions.csv` reserves 2.25 MiB for the firmware and 1.6875 MiB for LittleFS on a 4 MiB ESP32. The layout has been checked against the configured `esp32dev` 4 MiB target; verify the capacity of a different board before flashing.

After firmware upload:

1. Power the ESP32 and connect the phone to `LED-LAB`.
2. Keep the phone on the AP even if it reports that internet is unavailable, then open `http://192.168.4.1`.
3. Optionally enter the home router's SSID and password in **Home Wi-Fi**. The device reconnects with these credentials after normal firmware uploads; erasing the ESP32's flash also erases saved credentials.
4. To access LED LAB through the router, connect the phone or computer to the same home network and open `http://ledal.local`; use the DHCP IP shown in the Wi-Fi panel if the client does not resolve mDNS.
5. Choose an animation from the local library and press **Compile & Run**.

Closing the browser does not stop the ESP32 animation. A browser refresh restores local library/editor state but does not automatically restart the program.

FastLED 3.9.15 is used for WS2812B output and selects its ESP32 RMT transport under this ESP32 Arduino Core. The firmware performs current estimation and brightness limiting before handing GRB-ordered frames to FastLED.

## Animation DSL

Programs use the Previewer-style syntax:

```text
animation "Example" {
  state phase = 0;

  update(dt) {
    phase += dt;
  }

  render(i) {
    return hsv(fract(i / 300 + time * 0.05), 1, 0.5);
  }
}
```

Declarations are `animation`, `state`, optional `update(dt)`, and required `render(i)`. Statements include `let`, `return`, assignment and compound assignment, `if`/`else`, and bounded `for (let i = start; condition; i += step)` loops. Expressions include numeric/string/boolean literals, arrays, indexing, `-`, `!`, arithmetic, comparisons, logical operators, ternaries, and function calls. `else if` is not a separate construct; use nested `if` blocks.

Built-ins:

- Trigonometry: `sin`, `cos`, `tan`, `atan2`
- Arithmetic: `abs`, `sqrt`, `floor`, `ceil`, `round`, `exp`, `log`, `min`, `max`, `pow`
- General: `fract`, `clamp`, `lerp`, `smoothstep(edge0, edge1, value)`
- Random/noise: `random`, `randomRange`, deterministic hash-interpolated `noise(x[, y])`
- Color: `rgb`, `hsv`, `mix`, `gradient`
- Arrays: `array(length, initialValue)`, indexing, and indexed assignment

`time` is simulation time in seconds and `PI` is the mathematical constant. Each frame advances simulation time by `dt * timeScale`, passes unscaled `dt` once to `update`, then renders each configured pixel. Source size, token count, AST size, state count, array length, expression depth, loop iterations, instructions, and frame execution time are bounded. A candidate program replaces the active one only after successful parsing and state initialization. Failed frames are discarded, preserving the last transmitted frame.

The supported DSL is implemented natively in the firmware. The included browser app does not execute programs or stream pixel frames; the ESP32 is the only physical frame producer.

## Browser library

The versioned storage key is `ledlab.animations.v1`. The first visit seeds 17 examples. Create, edit/autosave, rename, duplicate, delete, select, import, and export are browser-local operations. Export a JSON library backup to transfer animations between browsers/phones. Corrupt stored data is not automatically overwritten; the interface offers a damaged-data download before an explicit reset.

## REST API

All routes use the device's same origin and `/api/v1/` prefix:

| Method | Route | Description |
|---|---|---|
| `GET` | `/api/v1/status` | Runtime, setup/home Wi-Fi state, DHCP IP and last disconnect reason, LED, current estimate, and frame metrics |
| `GET` | `/api/v1/capabilities` | Firmware/API/DSL versions and configured limits |
| `POST` | `/api/v1/wifi/scan` | Start an asynchronous scan for nearby Wi-Fi networks |
| `GET` | `/api/v1/wifi/scan` | Poll scan status and retrieve SSID bytes, signal strength, channel, and security |
| `POST` | `/api/v1/wifi` | Save `{ "ssidHex": "...", "password": "..." }` (or manual `{ "ssid": "...", "password": "..." }`) in NVS and connect |
| `POST` | `/api/v1/animation/compile-run` | Queue `{ "name": "...", "source": "..." }` for compilation |
| `GET` | `/api/v1/jobs/{jobId}` | Poll the compile result |
| `POST` | `/api/v1/animation/stop` | Stop; optional `{ "clear": true }` also clears LEDs |
| `POST` | `/api/v1/animation/step` | Queue one frame while stopped |
| `POST` | `/api/v1/settings` | Apply brightness, time scale, target FPS, and current budget |

Settings are validated and clamped by the device. Error responses include `error.code`, `error.message`, and source `line`/`column` when available. `/api/v1/status` reports measured scheduler, computation, and transfer metrics; `estimatedCurrentMa` is a model-based estimate, not a sensor reading.

## Troubleshooting and limitations

- If the page does not load, verify the phone remains connected to the no-internet AP and open `http://192.168.4.1` directly. The main page is embedded in firmware; LittleFS upload is not required for it.
- If home Wi-Fi does not connect, check that the router provides 2.4 GHz Wi-Fi and compare its scanned security mode with the router's settings; `LED-LAB` remains available for changing them. The network picker shows the reported RSSI, channel, and WPA mode.
- The Home Wi-Fi panel and serial monitor report the ESP32 disconnect reason and signal at disconnect. `AUTH_EXPIRE` (2) and `AUTH_LEAVE` (3) do not by themselves prove the password is wrong; `AUTH_FAIL` (202) and handshake timeout reasons are more specific authentication clues. For serial diagnostics, open the monitor at 115200 baud and look for `component:"wifi"` events.
- If a separate runtime asset (not the embedded `index.html`) is missing, run the LittleFS upload command after building and verify the filesystem is mounted in `/api/v1/status`.
- If no pixels light, verify the selected GPIO, strip direction, common ground, separate supply, level shifter, and strip color order.
- `OUT_OF_MEMORY` during compilation means the ESP32 could not allocate enough heap for the candidate program; reduce large state arrays or source complexity and retry.
- `TIME_LIMIT` means one frame exceeded the runtime's execution budget and was discarded; simplify per-pixel expressions, especially nested math, noise calls, and loops. Lowering target FPS does not make an individual frame cheaper.
- Serial logs run at 115200 baud. Upload and monitor ports are selected on the command line.
- 60 FPS is a target, not a guarantee. A 300-pixel WS2812B frame needs about 9 ms of raw wire time before reset/latch time and interpretation.
- Browser animation storage is per browser and origin. Private/incognito storage may not persist.
- Physical LED output, AP behavior on Android/iOS, power draw, and sustained frame rate have not been verified on attached hardware. Validate electrical design and measure the actual strip and power supply before unattended operation.
