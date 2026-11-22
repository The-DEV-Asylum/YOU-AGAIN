# AntiStalker companion app

Ionic + Angular + Capacitor app for Android and iOS. It talks to the board over plain local
HTTP, finds it again when it changes network, and makes the serial console optional.
App version **1.0.0**, targets firmware **0.5.0**, tolerates firmware **0.3.0+**.

No cloud. No account. No analytics. The only thing it fetches from the internet is
OpenStreetMap tiles, and only when you open the home-position map.

---

## What it does

- **Overview:** scanner state, uptime, packets/drops, risk signals, location epoch, and
  "active now" counts (seen in the last 2 minutes) instead of raw table sizes.
- **Devices:** tracking tags and ordinary BLE, with search and All / Needs review / Allowed filters.
  Details show filtered/peak RSSI, MAC + address type, sightings, minutes, places, owner state, and
  a distance with its band ("about 1-4 m"), because RSSI is not distance.
- **Allow / unallow** and a confirmation-protected **I am at a different location** button.
- **Trusted devices:** open a pairing window, read the 6-digit code, pair from the phone's
  Bluetooth settings. Rename/remove later.
- **GPS sharing (opt-in):** sends the phone's fix to the board (max every 30 s, or after 25 m)
  so it can start new location epochs on its own after ~150 m of real movement.
- **Home position (optional):** from phone GPS, a pin on the map, or typed in. Context only.
  It never silences alerts, because a tag in your bag comes home with you.
- **Home Wi-Fi setup** with live state. Credentials go straight to the board and are not stored by the app.
- **Device health:** heap, PSRAM, flash, chip temperature, battery (if wired), mDNS name, slot usage.
- **Auto refresh** every 3/5/10 s, on resume, on network change, and pull-to-refresh.
- **Auto discovery:** if the saved address dies, it tries the mDNS name, last home IP,
  `192.168.4.1` and `antistalker.local`, and keeps whichever answers.

---

## Connect

1. Power the board. Get the hotspot password from the serial log (`wifi`).
2. On the phone, join `AntiStalker-XXXX`. Say **Keep connection / Use anyway** when it whines about no internet.
3. Open the app, tap **Find the device automatically** (or **Save & check connection**).

After that you basically never type an address again. If the board moves between hotspot and
home Wi-Fi, the app finds it. The manual **Device address** field is still there for weird cases.

### Home Wi-Fi

1. While on the hotspot: **Setup & settings -> Home Wi-Fi**, enter SSID/password, toggle
   **Allow home Wi-Fi**, **Save home network**. Empty password = open network. Saving with the
   toggle off stores the credentials without using them.
2. The board keeps its hotspot up while it tries. Once connected, the hotspot turns off.
3. Join the same home network with your phone. The app re-finds the board by mDNS name or
   its remembered home IP.
4. Home Wi-Fi lost or disabled? The board brings its hotspot back. Rejoin it, the app will follow.

---

## Development

```sh
npm install
npm start          # browser preview (UI only, see below)
npm run build      # web assets in www/
npm run test:ci    # 41 Vitest tests
npm run lint       # currently 11 known template eqeqeq errors, see docs/LAB_NOTES.md
```

**The browser preview cannot really talk to the board.** The firmware sends no CORS headers,
on purpose. The app uses `CapacitorHttp`, which goes through the native HTTP stack on
Android/iOS and skips CORS there. Use a real device build to test against hardware.

### Android

JDK 21 recommended.

```sh
npm run cap:sync
cd android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Or `npx cap open android` and let Android Studio do the thinking. The manifest allows cleartext
HTTP (the board has no TLS) and network-state access. Location permissions are declared only for
GPS sharing. No Bluetooth permission: the board does the scanning, not the phone.

CI builds a debug-signed APK after the unit tests pass: see [`../.github/workflows/release.yml`](../.github/workflows/release.yml).

### iOS

On a Mac with Xcode:

```sh
npm run cap:sync
npx cap open ios
```

Pick a team and run on a device. `Info.plist` has the local-network usage description,
`NSAllowsLocalNetworking` for the board's HTTP API, and `NSLocationWhenInUseUsageDescription`
for GPS sharing.

**No Mac?** The [release pipeline](../.github/workflows/release.yml) builds an
**unsigned IPA** on a GitHub macOS runner on every push to `main` (after the tests pass). Then:

1. Download `AntiStalker-unsigned.ipa` from the latest GitHub Release.
2. Sign and install it with [Sideloadly](https://sideloadly.io/) or [AltStore](https://altstore.io/)
   using your own Apple ID (enable Developer Mode if asked).
3. Free Apple signing expires after **7 days**. Re-sign, or let AltStore refresh it.

The workflow stores no Apple credentials. Sideloadly/AltServer still need a Windows or macOS
computer; the cloud only replaces the *build* machine.

---

## Firmware API used

| App feature | Firmware API |
|---|---|
| Status and observations | `GET /api/status` |
| Confirm new location | `POST /api/moved` |
| Share a GPS fix | `POST /api/location` `{latitude, longitude, accuracy}` |
| Allow/unallow | `POST /api/allow?id=<id>&allowed=<0\|1>` |
| Home Wi-Fi | `POST /api/wifi` |
| Pairing window | `POST /api/trust/pair` `{seconds}`, `POST /api/trust/cancel` |
| Trusted devices | `POST /api/trust/name`, `POST /api/trust/remove` |
| Home position | `POST /api/home` `{latitude, longitude, accuracy?, source}`, `POST /api/home/remove` |

All POSTs send `X-AntiStalker: 1`. Full reference: [../docs/API.md](../docs/API.md).
Serial-only commands (`locate`, `clear`, `verbose`, `trustdump`, `devices on/off`) have no HTTP route.

---

## Layout

```text
src/app/core/antistalker.models.ts   API types, defaults, filters, helpers
src/app/core/device-api.service.ts   native HTTP client + response validation
src/app/core/monitor.service.ts      polling, discovery, actions, GPS sharing, settings
src/app/overview/                    live dashboard + location confirmation
src/app/devices/                     searchable observations
src/app/setup/                       connection, home Wi-Fi, trusted devices, home position, device info
src/app/shared/                      candidate cards, detail modal, Leaflet home map, pipes
android/  ios/                       native Capacitor projects
resources/                           vector icon masters + generate-assets.sh (needs ImageMagick)
```

---

## Safety

BLE adverts are unauthenticated. Rotation, spoofing, shared devices, radio coexistence and
unsupported protocols cause false positives and false negatives. RSSI is not distance. A
TRUSTED badge proves a device shared its key during your pairing, not who is carrying it.
Never confront someone based on an observation. If you are at risk, get somewhere safe and
contact local support or emergency services.
