# M5-AtomMatrix-Companion-v4-Satellite
A single-button Companion v4 satellite built for the M5 Atom Matrix. Includes 5x5 LED-matrix icons, external RGB LED output, WiFi configuration portal, OTA updates, and automatic MAC-based device ID. Designed for tally, triggers, and broadcast or live-event control surfaces.

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
- Matrix output is capped at 128/255 per channel; the external RGB LED retains its full 255/255 range
- Configurable matrix rotation: 0°, 90°, 180° or 270°
- Codebase aligned with the AtomS3 and AtomS3R versions for consistent behaviour
- Ready for release on GitHub and M5Burner

## Hardware Connections
Pin  Function  
G33  External LED Red  
G22  External LED Green  
G19  External LED Blue  
G23  External LED Ground  

Use a common-cathode RGB LED with G23 as ground.

# Recomended LED
AU: https://www.jaycar.com.au/tricolour-rgb-5mm-led-600-1000mcd-round-diffused/
USA: https://www.adafruit.com/product/302

## Installation and Usage
### Initial install with ESPHome Web (recommended)

1. Download `M5-AtomMatrix-Companion-v4-Satellite-factory.bin` from the latest GitHub release. This is the complete first-install image.
2. Connect the Atom Matrix with a USB **data** cable and open [ESPHome Web](https://web.esphome.io/).
3. Select **Connect**, choose the serial device, then choose **Install** and select the downloaded `.bin`.
4. Configure Wi-Fi and Companion after boot. Later updates use `http://<device-ip>:9999/update` without USB.

There are two release files: `*-factory.bin` is for the first USB flash only; `*.ino.bin` is the smaller application image for browser updates. The application image will not boot when flashed as a first install.

### Arduino development environment

1. Clone the repository.  
2. Open the M5AtomMatrix_Companionv4_SingleButtonSatellite.ino file in Arduino IDE.  
3. Select the correct board: M5 Atom (ESP32), using **Minimal SPIFFS (Large APPs with OTA)** partition scheme.  
4. Install libraries: M5Atom, WiFiManager, Preferences, ArduinoOTA.  
5. Upload firmware to the device.  
6. On first boot the LED matrix shows the boot sequence, then WiFi setup or Companion-wait status.  
7. In Companion v4: add the device under Surfaces and select the shown deviceID.  
8. Press the button to trigger Companion key events.  
9. Hold button for 5 seconds to open the WiFi config portal (SSID equals deviceID).  

## OTA Firmware Update
OTA updates are enabled.  
Use the deviceID as the upload hostname.  
ArduinoOTA password: companion-satellite. Browser updates are open by default and can be protected with an owner-selected password.

### Browser update (recommended)

1. Download `M5-AtomMatrix-Companion-v4-Satellite.ino.bin` from a GitHub release.
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
v1.3.2

## License
MIT License
