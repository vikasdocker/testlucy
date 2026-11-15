# ESP32-C3 Super Mini — WiFi Hotspot with OLED cat 🐱

Firmware for an **ESP32-C3 Super Mini** that broadcasts its own 2.4 GHz WiFi
hotspot, shows an animated cat face on a 128×64 OLED, and lets you join your home
WiFi from a phone browser.

![cat face preview](C3Hotspot/cat_preview.png)

## Hardware

| OLED pin | ESP32-C3 Super Mini | Notes |
|----------|---------------------|-------|
| VCC      | 3V3                 | |
| GND      | GND                 | |
| SCK/SCL  | GPIO 7              | |
| SDA      | GPIO 6              | |

- OLED: SSD1306 **or** SH1106, 128×64, I2C `0x3C`
- On-board LED: GPIO 8 (solid = client connected, slow blink = idle)
- BOOT button: GPIO 9 (menu)

## Hotspot

| | |
|---|---|
| SSID | `C3-F007` (derived from the chip MAC on first boot) |
| Password | `hotspot1234` |
| Dashboard | http://192.168.4.1 or http://hotspot.local |
| Band | 2.4 GHz, channel 6, max 4 clients, WPA2 |

Name and password can be changed from the dashboard (`/config`) and are stored in
NVS.

## BOOT-button menu

Tap BOOT to open the menu, tap to scroll, **hold ~0.7 s to select**:

```
MENU     hold=select
> WiFi status
  Scan networks
  Home WiFi
  AP: ON  (turn off)
  Web settings
  Info screens
  Exit to cat
```

- **WiFi status** — home network, state, IP, RSSI
- **Scan networks** — nearby 2.4 GHz networks with signal
- **Home WiFi** — reconnects to the saved router (or tells you where to enter it)
- **AP: ON/OFF** — two-step confirm, so a stray long press can't kill the hotspot
- **Web settings** — `192.168.4.1/wifi`
- **Info screens** — connection, clients, system pages
- **Exit to cat** — back to the cat

Long press on the info screens returns to the cat.

## Joining your home WiFi

1. Join `C3-F007` from your phone
2. Open **http://192.168.4.1/wifi**
3. Tap your router in the list, type the password, press **Connect**

The board runs AP+STA: the hotspot stays up while it joins the router, and the
dashboard is then also reachable on the IP your router assigns. Credentials are
saved and retried on boot.

## Web endpoints

| Path | Purpose |
|------|---------|
| `/` | dashboard: status, clients, heap, uptime |
| `/wifi` | home WiFi setup + network scan |
| `/wifisave` | save router credentials and connect |
| `/config` | change hotspot SSID/password |
| `/display` | panel offset: SH1106 (2 px) ↔ SSD1306 (0 px) |
| `/ap?state=on\|off` | access point on/off |
| `/reboot` | restart |

## Display notes

The panel is always driven in **page-addressing mode** with an explicit column
offset, so both controllers work:

- 2 px offset → SH1106 (typical 1.3" module, 132×64 RAM)
- 0 px offset → SSD1306 (0.96")

This matters: SSD1306-style horizontal-addressing writes on an SH1106 leave a
screen full of random noise, because SH1106 has no such mode. Toggle live from
`/display` if the image sits 2 px off one edge.

`C3Hotspot/cat_preview.py` renders the same cat geometry to a PNG on a PC
(requires Pillow) so the artwork can be tweaked without reflashing.

## Build and flash

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "Adafruit SSD1306" "Adafruit GFX Library" "Adafruit BusIO"

arduino-cli compile --fqbn esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc C3Hotspot
arduino-cli upload -p COM6 --fqbn esp32:esp32:nologo_esp32c3_super_mini:CDCOnBoot=cdc C3Hotspot
```

Board: **Nologo ESP32C3 Super Mini**, *USB CDC On Boot: Enabled*. Sketch uses
~83 % of the 1.3 MB app partition.

## Known limitations

- **No internet for hotspot clients.** The board can join your router, but it does
  not route/NAT traffic between AP and STA, so phones on `C3-F007` have no
  internet. Adding NAT is the next step if that's needed.
- **RF performance depends on the board.** If the SSID is hard to find, check the
  antenna: some clones have an IPEX connector with no antenna fitted, and the 3V3
  regulator is marginal when the OLED is also powered from it.
- AP on/off is not persisted, so a reboot always brings the hotspot back (and a
  60 s guard restores it automatically if there is no home-WiFi uplink).


## Wi-Fi broadcast troubleshooting (firmware patch)

The current firmware starts AP+STA with a conservative 8.5 dBm TX power,
checks the return value of `WiFi.softAP()`, and retries in AP-only mode if
AP+STA startup fails. It no longer runs an active Wi-Fi scan five seconds
after boot, because that scan can temporarily interrupt hotspot beaconing.
Serial Monitor at 115200 prints the AP start result, SSID, IP, channel, TX
power and mode. The OLED wiring in the sketch is SDA GPIO7 / SCL GPIO6.

## PlatformIO build (ESP32-C3 Super Mini)

The project now includes a PlatformIO environment for the existing `C3Hotspot/C3Hotspot.ino` sketch.

1. Open this repository folder in VS Code with the PlatformIO extension installed.
2. Select environment **esp32-c3-supermini**.
3. Build with **PlatformIO: Build**.
4. Connect the ESP32-C3 Super Mini by USB and choose **PlatformIO: Upload**.
5. Open **PlatformIO: Monitor** at 115200 baud.

### OLED / wiring preserved

- OLED VCC → 3V3
- OLED GND → GND
- OLED SCL → GPIO 7
- OLED SDA → GPIO 6
- I2C address: `0x3C`
- Existing SH1106 1.3-inch panel offset and page-mode flush routine are retained.

The OLED home page now renders Lucy's animated 3D-style monochrome AI face with blinking, gaze movement, a subtle idle bob, and a layered HUD/helmet outline. The existing hotspot, saved Wi-Fi credentials, AP+STA connection logic, captive portal, BOOT-button menu, and web routes are left intact.
