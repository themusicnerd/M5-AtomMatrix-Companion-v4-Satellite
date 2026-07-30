# M5-AtomMatrix-Companion-v4-Satellite
A single-button Companion v4 satellite built for the M5 Atom Matrix. Separate Wi-Fi + external LED and Atomic PoE/W5500 firmware variants are provided.

## Features
- Uses the M5 Atom Matrix (ESP32)
- 5x5 matrix icon system for Boot, WiFi Setup, Waiting for Companion, Ready and Error states
- External RGB LED control on G33 (Red), G22 (Green), G19 (Blue) with G23 Ground
- External LED mirrors Companion key colour with brightness scaling
- WiFiManager configuration portal (hold button for 5 seconds)
- OTA firmware updates via ArduinoOTA
- Auto-generated deviceID “M5ATOM_xxxxxxxxxxxx” based on full MAC address
- Companion API support: KEY-STATE, COLOR, BRIGHTNESS, TEXT, PING, DEVICE-ADD
- 3×5 pixel text font: short labels are centred and long labels scroll across the 5×5 matrix
- Colour-only fallback: send an empty text label to use the matrix as a solid tally-colour indicator
- Matrix colour data and the external RGB LED both use their full 0–255 range; Companion brightness 0–100 maps to the Matrix controller's safe 0–20/255 range
- Configurable matrix rotation: 0°, 90°, 180° or 270°
- Atomic PoE Base build with W5500 Ethernet, DHCP, wired setup/REST page and browser firmware updates
- Codebase aligned with the AtomS3 and AtomS3R versions for consistent behaviour
- Ready for release on GitHub and M5Burner

## Hardware Connections
Pin  Function  
G33  External LED Red  
G22  External LED Green  
G19  External LED Blue  
G23  External LED Ground  

Use a common-cathode RGB LED with G23 as ground.

The external LED and Atomic PoE Base use the same four GPIOs, so they cannot be used together. Install the normal firmware for Wi-Fi + LED, or the `-poe` firmware for Ethernet + PoE. The PoE variant disables all external LED access.

# Recomended LED
AU: https://www.jaycar.com.au/tricolour-rgb-5mm-led-600-1000mcd-round-diffused/
USA: https://www.adafruit.com/product/302

## Installation and Usage
### Initial install with ESPHome Web (recommended)

1. Download the matching `M5-AtomMatrix-Companion-v4-Satellite-wifi-factory.bin` or `M5-AtomMatrix-Companion-v4-Satellite-poe-factory.bin` from the latest GitHub release. This is the complete first-install image.
2. Connect the Atom Matrix with a USB **data** cable and open [ESPHome Web](https://web.esphome.io/).
3. Select **Connect**, choose the serial device, then choose **Install** and select the downloaded `.bin`.
4. Configure Wi-Fi and Companion after boot. Later updates use `http://<device-ip>:9999/update` without USB.

Each network variant has two release files: `*-factory.bin` is for the first USB flash only; `*.ino.bin` is the smaller application image for browser updates. The application image will not boot when flashed as a first install.

The v1.3.10 release provides both network variants:

- `*-wifi-factory.bin` / `*-wifi.ino.bin` — Wi-FiManager, mDNS and external RGB LED.
- `*-poe-factory.bin` / `*-poe.ino.bin` — Atomic PoE Base, W5500 DHCP and wired setup at `http://<dhcp-ip>:9999/`.

The PoE build uses G22=SCK, G23=MISO, G33=MOSI and G19=CS. Its DHCP address is printed over USB serial at 115200 baud.

### Arduino development environment

1. Clone the repository.  
2. Open `M5-AtomMatrix-Companion-v4-Satellite.ino` in Arduino IDE.
3. Select the correct board: M5 Atom (ESP32), using **Minimal SPIFFS (Large APPs with OTA)** partition scheme.  
4. Install libraries: M5Atom, WiFiManager and M5-Ethernet. Preferences and ArduinoOTA are supplied by the ESP32 board package.
5. Build normally for Wi-Fi + LED. Define `ATOMIC_POE_BUILD` for the Atomic PoE/W5500 variant.
6. Upload firmware to the device.
7. On first boot the LED matrix shows the boot sequence, then WiFi setup or Companion-wait status.
8. In Companion v4: add the device under Surfaces and select the shown deviceID.
9. Press the button to trigger Companion key events.
10. Hold button for 5 seconds to open the WiFi config portal (SSID equals deviceID).

## OTA Firmware Update
OTA updates are enabled.  
Use the deviceID as the upload hostname.  
ArduinoOTA password: companion-satellite. Browser updates are open by default and can be protected with an owner-selected password.

### Browser update (recommended)

1. Download the matching `*-wifi.ino.bin` or `*-poe.ino.bin` application image from a GitHub release.
2. Browse to `http://<device-ip>:9999/update` on the same Wi-Fi network.
3. Updates are open by default. Use the optional protection form on that page to set a password; once set, sign in as `admin` with that password.
4. Select the `.bin` and wait for the automatic reboot. Never remove power while the upload is in progress.

The AtomMatrix release uses the **Minimal SPIFFS (Large APPs with OTA)** partition scheme; keep that partition scheme when building firmware. Only the release application `.bin` can be installed through the browser.

### 5x5 status indicator

The top-left matrix pixel remains a connection indicator while the rest of the panel shows the Companion tally colour: blue = waiting/connecting, green = connected, orange = Wi-Fi setup, red = error.

### Text and colour-only modes

The Atom Matrix now advertises `TEXT=true` to Companion. Text sent with a key state is base64-decoded and rendered using a compact 3×5 font. Labels up to one glyph wide are centred; longer labels scroll continuously. The glyph is drawn in a contrasting colour over the Companion `COLOR` value.

For background-colour-only mode, set the key text to empty. The panel then returns to the normal solid tally colour (with its connection-status pixel).

Matrix rotation can be set in the Wi-Fi configuration portal or with `POST /api/settings`, for example `{"rotation":90}`. Valid values are 0, 90, 180 and 270. `GET /api/settings` reports the saved rotation alongside brightness.

## Troubleshooting
Matrix LEDs blank: confirm Companion is sending COLOR or BITMAP data.  
External LED not working: verify wiring and that the LED is common-cathode.  
DeviceID shows zeros: clear Preferences or reboot to reinitialise MAC.  
Cannot connect to Companion: check host IP and port in WiFi portal settings.  
Brightness mismatches: ensure Companion is sending BRIGHTNESS commands.

## Version
v1.3.10

## License
MIT License
