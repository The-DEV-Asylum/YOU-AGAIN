# AntiStalker firmware

ESP32-S3 firmware: passive BLE scanner, two detection engines, trusted-device pairing,
Wi-Fi hotspot / home Wi-Fi, embedded dashboard and JSON API. Version **0.5.0**.

Target: **ESP32-S3-WROOM-1-N16R8** (16 MB flash, 8 MB OPI PSRAM). Other S3 boards may work
if you fix the memory settings in `platformio.ini`. The boot log warns you if flash/PSRAM
do not look like N16R8.

How it works: [../docs/ENGINEERING.md](../docs/ENGINEERING.md).
What can go wrong: [../docs/THREAT_MODEL.md](../docs/THREAT_MODEL.md).

---

## Build, flash, monitor

Install [PlatformIO](https://platformio.org/install), plug in the board, then:

```sh
pio run -e esp32s3_n16r8                    # build
pio run -e esp32s3_n16r8 --target upload    # flash
pio device monitor -e esp32s3_n16r8         # serial, 115200 baud
pio test -e native                          # 46 unit tests, no board needed
```

Or skip the toolchain and flash the file from the latest GitHub Release:

```sh
esptool.py --chip esp32s3 write_flash 0x0 antistalker-firmware-merged.bin
```

Split images, if you prefer: `bootloader.bin @ 0x0`, `partitions.bin @ 0x8000`,
`boot_app0.bin @ 0xe000`, `firmware.bin @ 0x10000`.

Things that will bite you:

- **Serial goes to UART0**, i.e. the board's WCH USB-UART bridge (`ARDUINO_USB_CDC_ON_BOOT=0`).
  Using the native USB port instead? Set it to `1`, or enjoy the silence.
- Opening the serial port may **reset** the board. Wait for the banner before typing.
  A reset wipes observation history, not the saved passwords.
- If auto-reset into download mode fails, do the `BOOT` + `RESET` button dance.
  The application itself never uses the `BOOT` button.
- Repeated `BROWNOUT_RST` = bad cable or weak supply. Not a firmware bug. Probably.
- Versions are pinned: `espressif32@6.12.0`, `NimBLE-Arduino@2.5.2`, `ArduinoJson@7.4.3`.

### PlatformIO in a local venv

If `pio` is not on your `PATH` and you installed it next to the code:

```sh
export PLATFORMIO_CORE_DIR="$PWD/.platformio-core"
.venv/bin/pio run -e esp32s3_n16r8
.venv/bin/pio test -e native
```

(Both folders are git-ignored.)

### Optional: battery monitoring

The bare dev board has no idea it runs on a battery. If yours sits behind a voltage divider,
uncomment in `platformio.ini`:

```ini
-DANTISTALKER_BATTERY_PIN=4        ; ADC GPIO
-DANTISTALKER_BATTERY_DIVIDER=2.0  ; battery volts / pin volts (100k/100k)
```

Keep the pin below ~3.1 V at full charge. The percentage is a linear 3.5-4.2 V guess for 1S Li-ion.

---

## First boot

The log prints the hotspot name and password:

```text
AntiStalker 0.5.0 - passive BLE tracker and co-presence monitor
...
Wi-Fi fallback AP: ssid=AntiStalker-1A2B password=<16 random chars> state=on
Wi-Fi AP URL: http://192.168.4.1 clients=0
```

Join it from your phone (say "keep connection" when it complains about no internet) and open
`http://192.168.4.1`, or use the companion app.

The password is generated once from the hardware RNG and stored in NVS. It survives reboots
and firmware updates. `wifi` prints it again. Only a full flash erase makes a new one.

---

## Serial console

```text
help                   Show commands
list                   Show tracker and personal/general BLE candidates
devices                Show only personal/general BLE candidates
devices on|off         Enable/pause new general-device observations
moved                  Confirm arrival at a new physical location
allow <id>             Suppress alerts for your own device (RAM only)
unallow <id>           Re-enable alerts
locate <id>            Live RSSI for one candidate (hot/cold game)
locate off             Stop RSSI output
verbose on|off         Print every advert with its raw payload (very chatty)
wifi                   AP/home state, AP password, mDNS name and URLs
wifi on|off            Stop/start Wi-Fi and web access (BLE keeps scanning)
stats                  Scan, drops, memory, temperature, GPS and alert stats
clear                  Clear history, location epochs and RAM allow lists
trustdump              Trusted devices INCLUDING their IRKs (serial only, on purpose)
```

Log prefixes, so you know what you are looking at:

| Prefix | Meaning |
|---|---|
| `[NEW]` | a tracker-capable device appeared (not proof of anything) |
| `[DEVICE]` | a labelled ordinary device appeared |
| `[ALERT]` | something escalated to `PERSISTENT`, `FOLLOWING` or `CO-PRESENT` |
| `[STATE]` / `[DEVICE-STATE]` | a level went down or changed without alerting |
| `[TRUST]` | pairing and trusted-device events |
| `[RSSI]` | `locate` output |
| `[WIFI]` `[WEB]` `[GPS]` `[RADIO]` `[BOOT]` | the plumbing talking |

Suggested field test:

1. Boot it, let it collect a baseline.
2. Pair your phone/watch (dashboard or app). `allow` your other own devices.
   Allow entries are RAM only and rotating devices may need it again.
3. Travel normally. At the destination: `moved` (or the button, or GPS sharing in the app).
4. Look at `PERSISTENT`, `FOLLOWING`, `CO-PRESENT`.
5. For something interesting: `locate <id>` and move slowly. Check your belongings, safely.
6. If you feel at risk: get somewhere safe and contact local support or emergency services.
   Do not confront anyone over BLE output.

---

## Layout

```text
src/main.cpp                       boot, loop, serial console
src/BleMonitor.*                   passive scanner, RTOS queue, dispatch to the engines
src/WebPortal.*                    AP/home Wi-Fi, mDNS, dashboard, JSON API
src/TrustManager.*                 pairing window, IRK capture, NVS persistence
lib/AntiStalker/src/
  TrackerClassifier.*              tracker signatures (Apple, Google, Samsung, DULT, Tile, ...)
  PersonalDeviceClassifier.*       phones/watches/audio/Continuity/Fast Pair labels + fallback
  DetectionEngine.*                identity, history, retention, alert state machine
  TrustResolver.*                  Bluetooth ah() RPA resolution against bonded IRKs
  DistanceEstimate.*               RSSI -> rough meters + honest bands
  tiny-aes/                        vendored tiny-AES-c (public domain), used only for ah()
test/test_core/test_main.cpp       native Unity tests
platformio.ini                     N16R8 + native environments
```

`lib/AntiStalker` must stay Arduino-free: the native test build compiles only that library.

---

## Protocol references

- [Bluetooth SIG Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers): Appearance values, company and service IDs.
- Bluetooth Core Specification, Vol 3, Part H, 2.2.2: the `ah()` random address hash.
- [IETF DULT working group](https://datatracker.ietf.org/group/dult/about/) and the [accessory protocol draft](https://www.ietf.org/archive/id/draft-ietf-dult-accessory-protocol-01.html): `FCB2`, near-owner state. An Internet-Draft, not a final RFC.
- [Google Find Hub Network Accessory Specification](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn#advertised-frames) and [Fast Pair Provider advertising](https://developers.google.com/nearby/fast-pair/specifications/service/provider).
- [OpenHaystack ESP32 firmware](https://github.com/seemoo-lab/openhaystack/blob/main/Firmware/ESP32/main/openhaystack_main.c): Apple Offline Finding advert layout.
- [AirGuard](https://github.com/seemoo-lab/AirGuard) and [Kismet tracker parsers](https://github.com/kismetwireless/kismet/tree/master/bluetooth_parsers): other defensive scanners, used to cross-check vendor formats.
- [Discontinued Privacy: Apple Continuity analysis](https://petsymposium.org/2020/files/papers/issue1/popets-2020-0003.pdf): Nearby Info and Proximity Pairing layouts.
- [Samsung SmartTag / Offline Finding analysis](https://www.usenix.org/system/files/sec23winter-prepub-498-yu.pdf).
- [Espressif RF coexistence](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html).

Third party: `lib/AntiStalker/src/tiny-aes/` is [tiny-AES-c](https://github.com/kokke/tiny-AES-c),
public domain (Unlicense), left untouched.
