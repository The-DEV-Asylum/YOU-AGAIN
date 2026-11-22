# AntiStalker

### A 10 euro microcontroller that is paranoid on your behalf.

Welcome to another **DEV Asylum** project.

This time we took an ESP32-S3, taught it to listen to every Bluetooth Low Energy
device around you, and asked it one question over and over again:

> "Is that thing following me, or am I just on a crowded train?"

It does this **passively** (it never transmits a single scan request), **locally** (no
cloud, no account, no telemetry, no "sign in with Google"), and with a companion
phone app so you do not have to stare at a serial terminal on the bus. Even tho
staring at a serial terminal on the bus is a valid lifestyle.

> [!NOTE]
> Not because the world needed another BLE scanner.
>
> Not because phones do not already have tracker alerts (some do, sometimes, kinda).
>
> Because "detect a tracker that is trying not to be detected, using a chip that
> costs less than a kebab" turns out to be a great excuse to learn BLE privacy,
> RPA resolution, AES, radio coexistence, state machines and threat modelling.

> [!IMPORTANT]
> This is the one part of this README that is not a joke.
>
> AntiStalker reports **radio observations and risk signals**. It does not know who
> carries a device or why. A phone, a headset or a tag near you is NOT, by itself,
> evidence of stalking. If you feel at risk, get somewhere safe and contact local
> support services or emergency services. Do not confront anyone based on what a
> microcontroller printed.

---

## The Asylum standard

| Question | Answer |
|---|---|
| Does it work? | **Yes.** Scanner, classifiers, dashboard, app, pairing and auto-location run on real hardware. The full "FOLLOWING across two real places" alert still needs a proper field trip, see [what is not proven yet](#what-is-not-proven-yet). |
| Is it useful? | **Suspiciously, yes.** We apologise. This was supposed to be useless. |
| Did we learn something? | **Absolutely.** Mostly about byte order. It is always byte order. |

---

## What is in the box

```text
final_release/
|-- firmware/           ESP32-S3 firmware (PlatformIO + Arduino + NimBLE)
|   |-- src/            board-specific: scanner, Wi-Fi/web, pairing, console
|   |-- lib/AntiStalker portable brain: classifiers, detection engine, IRK resolver
|   `-- test/           46 native unit tests, run on your laptop
|-- app/              Companion app (Ionic + Angular + Capacitor, Android & iOS)
|-- docs/
|   |-- ENGINEERING.md  how it actually works, with diagrams
|   |-- THREAT_MODEL.md security, privacy, and "what if the stalker reads this"
|   |-- LAB_NOTES.md    how it was tested, what broke, what we learned
|   `-- API.md          HTTP API reference
`-- .github/workflows/  CI: firmware .bin, Android .apk, iOS .ipa
```

Hardware needed: **one ESP32-S3-WROOM-1-N16R8 dev board and a USB data cable.**
That is it. No wiring, no soldering, no display. A power bank works after flashing.
(Optional: a voltage divider if you want battery readings. See the firmware README.)

---

## How it works (the 30 second version)

```text
   every BLE advert in the air
              |
              V
   +-----------------------+
   | passive BLE scan      |   1M + Coded PHY, 50% duty, never transmits
   +-----------------------+
              |
              V
   is it one of MY paired devices?  --yes-->  TRUSTED, never alerts
   (IRK resolves its rotating address)
              | no
              V
   +-----------------------+        +-----------------------------+
   | tracker classifier    |  no    | personal device classifier  |
   | AirTag/FindMy, Google |------->| phone, watch, earbuds,      |
   | Find Hub, SmartTag,   |        | Fast Pair, Continuity, or   |
   | Tile, Chipolo, DULT.. |        | "Unclassified BLE"          |
   +-----------------------+        +-----------------------------+
              |                                   |
              V                                   V
      tracker engine                        device engine
   NEARBY -> PERSISTENT -> FOLLOWING     NEARBY -> CO-PRESENT
              |                                   |
              +-----------------+-----------------+
                                |
                                V
             serial console / web dashboard / phone app
```

The key idea: **"near me for a long time" is weak evidence. "Near me at several
different places" is strong evidence.** Your neighbour's phone is near you all
night. It does not also take the same train and walk into the same supermarket.

So the board counts **location epochs**: every time you confirm "I am somewhere
else now" (a button, a serial command, or automatically from your phone's GPS
after ~150 m of real movement), the epoch counter goes up. A device seen at 2+
epochs, with enough sightings, gets escalated.

The board never stores *where* you were. Only "place #1, place #2, place #3".
Privacy by ignorance.

Full details, thresholds and diagrams: [docs/ENGINEERING.md](docs/ENGINEERING.md).

---

## Alert levels

| Level | Who | Means | Should you worry? |
|---|---|---|---|
| `NEARBY` | everyone | seen at least once | No. That is just Bluetooth existing. |
| `PERSISTENT` | trackers only | 10+ min, 4+ sightings, 3+ distinct minutes | Maybe look around. Could be your colleague's keys. |
| `FOLLOWING` | trackers | seen twice+ at each of 2+ confirmed places | Check your bag, coat, car. Safely. |
| `CO-PRESENT` | phones/other | same as FOLLOWING, but by MAC address | Weaker evidence. Exclude your own stuff first. |

Tags that say "I am with my owner" (DULT near-owner bit, Samsung connected state)
stay `NEARBY`. Your own **trusted** (paired) devices are always `NEARBY`, forever.

---

## Quick start

### 1. Flash the firmware

Grab `antistalker-firmware-merged.bin` from the
[Firmware workflow](.github/workflows/firmware.yml) artifacts (or a Release), then:

```sh
esptool.py --chip esp32s3 write_flash 0x0 antistalker-firmware-merged.bin
```

Or build it yourself:

```sh
cd firmware
pio run -e esp32s3_n16r8 --target upload
pio device monitor -e esp32s3_n16r8      # 115200 baud
```

The boot log prints the hotspot name (`AntiStalker-XXXX`) and its password.
The password is random, generated once, and kept in flash. Type `wifi` to see it again.

### 2. Connect

- **Browser:** join `AntiStalker-XXXX`, accept "no internet, keep connection anyway",
  open `http://192.168.4.1`. Done. The dashboard is served by the board itself.
- **Phone app:** install the APK (Android) or sideload the IPA (iOS), join the
  hotspot, tap **Find the device automatically**.

### 3. Use it properly

1. Let it scan for a bit.
2. **Pair your own phone/watch** once (Setup -> Trusted devices) so it stops
   reporting you to yourself.
3. `allow` your other own stuff: earbuds, car, the smart fridge that for some reason has Bluetooth.
4. Turn on **Share this phone's GPS** in the app, or press **I am at a different
   location** when you genuinely change place. Another room does not count.
5. Go about your day. Check for `FOLLOWING` / `CO-PRESENT`.
6. Found one? `locate <id>` (serial) shows live RSSI, so you can play hot/cold.
   Remember: RSSI is not a tape measure.

More: [firmware/README.md](firmware/README.md) and [app/README.md](app/README.md).

---

## What it can see (and what it absolutely cannot)

**Can see:** anything that advertises over BLE. Apple Find My (AirTag and friends),
Google Find Hub, Samsung SmartTag, Tile, Chipolo, Pebblebee, DULT-compatible tags,
phones, watches, earbuds, and the endless crowd of "Unclassified BLE" mystery boxes.

**Cannot see:**

- Bluetooth Classic devices. The ESP32-S3 radio is **BLE only**. Physics, not laziness.
- GPS trackers with a SIM card. They talk to cell towers, not to us.
- Anything powered off, not advertising, or using a proprietary radio.
- Names that are only sent in scan responses (we never ask, we are passive).

Things that will fool it, in both directions: MAC address rotation, spoofed adverts,
shared devices, a busy RF environment, Wi-Fi coexistence. All of it is documented in
[docs/THREAT_MODEL.md](docs/THREAT_MODEL.md), because "trust me bro" is not a threat model.

---

## What is not proven yet

Rule #1 of the Asylum is "it must work". So, honestly:

- **Verified on real hardware:** BLE scanning with real Find Hub, Apple Continuity,
  audio, Fast Pair and unclassified devices. WPA2 hotspot, password persistence,
  home Wi-Fi switch and fallback, mDNS, trusted-device pairing with a real phone (that is
  where the IRK capture test vector comes from), and zero queue drops with Wi-Fi on.
- **Verified by unit tests:** every escalation threshold, including `FOLLOWING`
  across two location epochs (46 native tests, run in CI).
- **Not yet verified in the field:** a real tag actually travelling with someone across
  two real places and raising `FOLLOWING` end to end. It needs a genuine multi-location
  field test, and we are not going to pretend we did one.

Everything that broke during development (and there was plenty) is in
[docs/LAB_NOTES.md](docs/LAB_NOTES.md).

---

## Builds (CI)

| Workflow | Output | Trigger |
|---|---|---|
| `firmware.yml` | `antistalker-firmware-merged.bin` (flash at `0x0`) + split images | push to `firmware/**`, PRs, tags, manual |
| `android.yml` | `AntiStalker-debug.apk` (debug-signed, sideloadable) | push to `app/**`, PRs, tags, manual |
| `ios.yml` | `AntiStalker-unsigned.ipa` (sign it yourself with Sideloadly/AltStore) | tags, manual |

Push a tag like `v0.5.0` and all three files get attached to a GitHub Release.

---

## Disclaimer

This is an educational, experimental project. It is a heuristic screening toy that
happens to be built properly. It is **not** a certified anti-stalking product, not a
replacement for your phone's built-in unwanted-tracker alerts, and not a replacement
for professional help or a proper physical inspection.

Do not deploy experimental projects as safety systems just because the README made you laugh.

Built in the DEV Asylum. Tested. Documented. Slightly over-engineered. As intended.
