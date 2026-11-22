# Engineering notes

### Or: how a microcontroller decides whether to be worried.

This is the deep dive. If you only want to flash it and go, the main
[README](../README.md) is enough. If you want to know why every number is what it is,
pour yourself something and read on.

---

## 1. The architecture

```text
+------------------------------------------------------------------+
| ESP32-S3 N16R8                                                   |
|                                                                  |
|  NimBLE host task             Arduino loop() task                |
|  -----------------            ----------------------------       |
|  onResult()                   BleMonitor::poll()  (max 16/pass)  |
|    copy adv  ---> [queue 24] --->  TrustResolver (IRK / ah())    |
|    count drops                     TrackerClassifier             |
|                                    PersonalDeviceClassifier      |
|                                    DetectionEngine x2            |
|                                                                  |
|                               TrustManager::poll()  (pairing)    |
|                               WebPortal::handle()   (HTTP+Wi-Fi) |
|                               pollSerial()          (console)    |
|                               prune()  once a minute             |
+------------------------------------------------------------------+
        |  Wi-Fi: fallback AP 192.168.4.1  or  home STA + mDNS
        V
  browser dashboard (served by the board)   /   companion app (Capacitor)
```

Two hard rules shaped all of it:

1. **The radio callback does nothing clever.** `onResult()` runs inside the NimBLE
   host task. It copies the advert into a fixed-size struct, pushes it into a FreeRTOS
   queue with zero wait, and leaves. Full queue? Drop it and count it (`dropped` in
   `stats`). Blocking in there would stall the BLE stack, which is worse than losing one packet.
2. **The brain is portable.** Everything in `firmware/lib/AntiStalker` is plain C++11
   with no Arduino, no NimBLE and no heap allocation. That is what lets 46 unit tests
   run natively on a laptop (and in CI) in about three seconds.

---

## 2. Parsing adverts without getting owned

A BLE advertisement payload is a sequence of AD structures:

```text
 +--------+---------+-------------------+--------+---------+-------
 | len=N  | AD type | value (N-1 bytes) | len=M  | AD type | ...
 +--------+---------+-------------------+--------+---------+-------
```

Every single advert is attacker-controlled input. Anyone with a laptop can broadcast
anything. So both classifiers:

- validate `len` against the remaining buffer **before** reading a single value byte;
- stop at the first malformed structure instead of trying to be smart;
- bounds-check nested formats too (Apple Continuity is TLVs inside manufacturer data);
- sanitise device names to printable ASCII before they reach a terminal (escape
  sequences in a BLE name are a real, very silly, attack).

There are dedicated tests for truncated, zero-length and garbage payloads.

---

## 3. Who is who: tracker signatures

| Family | Signature | Gotcha |
|---|---|---|
| Apple Find My | company `004C`, type `12`, length `19` (full) or `02` (short) | Short frames with device class 0 are iPhones/iPads/Macs, not tags. Excluded. Status byte not hashed. |
| Google Find Hub | service data `FEAA`, frame `40`/`41`, EID of exactly 20 or 32 bytes | `FEAA` is also plain Eddystone. Strict lengths keep museum beacons out. |
| Samsung SmartTag | service data `FD5A`, offline-finding frame | States 1-3 offline (escalates), 4-6 connected (suppressed), others unknown. |
| DULT | service data `FCB2`, network ID + near-owner bit | The IETF draft standard. Near-owner bit is read but never hashed. |
| Tile | `FEED` service data or UUID | No reliable passive owner-proximity info. |
| Chipolo | `FE33` | Newer models may show up as Apple/Google/DULT instead. |
| Pebblebee | UUID `FA25` | Same as Chipolo. |

When one advert matches several things, a priority table decides (DULT wins, because
its standard near-owner state is the most useful info we can get).

### Identity: the MAC address lies

Trackers rotate their MAC address on purpose. If we keyed candidates by MAC, every
rotation would create a "new" tag and nothing would ever escalate. So for trackers
with an identity payload, the candidate ID is:

```text
id = FNV-1a( family || FNV-1a(protocol-specific identity bytes) )
```

For Apple that is the key material after the status byte, for Google the EID, for
Samsung the 8-byte privacy ID, and so on. Status/battery bytes are skipped, so a
battery change does not spawn a new candidate.

FNV-1a is not cryptographic and does not need to be. A collision merges two candidates,
which is a false positive we can live with on a table of 64.

**Known limitation:** when a tracker rotates its MAC AND its payload identifier at the
same time, it splits into two candidates. That is the tracker's privacy design working
as intended, and also our biggest false-negative source.

### Personal devices: deliberately dumb

Phones, watches and earbuds get labels only when they self-declare (Bluetooth
Appearance values, known Apple/Google/Samsung frames). Everything else is
"Unclassified BLE", and **it is still tracked**. We never drop a device because we
cannot name it.

Their candidate ID is **MAC + address type only**. Their payloads change every few
seconds (counters, battery, state), so fingerprinting them would create a new
candidate per advert. Consequence: a phone using rotating private addresses
(roughly every 15 min) gets split. Again, that is privacy working as designed.

---

## 4. The detection engine

Each engine is a fixed table of 64 `Candidate` slots. No heap.

```text
                 +-------------------------------------------+
  new sighting ->| NEARBY                                    |
                 +-------------------------------------------+
                    |                                   |
     trackers only: |                                   | 2+ confirmed locations,
     >= 10 min,     |                                   | >= 2 sightings at each,
     >= 4 sightings,|                                   | >= 4 sightings total,
     >= 3 distinct  |                                   | >= 60 s observed
     minutes        V                                   V
                 +------------+                  +---------------------------+
                 | PERSISTENT |----------------->| FOLLOWING                 |
                 +------------+                  | (CO-PRESENT for non-tags) |
                                                 +---------------------------+

  near-owner frame or TRUSTED device -> forced back to NEARBY (history kept)
```

Why "at least 2 sightings at each location"? One stray packet at the second place
should not count. A single passing car could do that.

Why no `PERSISTENT` for phones? Because you live next to people. A wall is not a
security boundary for 2.4 GHz.

### Location epochs

The engine has no idea where you are. It only has a counter. Every candidate remembers
the last epoch it was seen in; when it shows up in a new one, its `locations` count goes up.

The epoch advances when:

- you run `moved` on the serial console, or press the button in the dashboard/app; or
- the app shares phone GPS (opt-in) and the phone has moved **150 m or more** from
  the epoch's anchor fix, with a reported accuracy of **100 m or better**.

The accuracy gate exists because indoor GPS happily "moves" you 300 m while you sit
on the sofa. Fixes with accuracy 0 (unknown) never auto-advance. Manual confirmation
re-anchors the GPS logic, so the next fix cannot immediately trigger a second epoch.

### Retention: why counts do not stay stuck at 64

With one flat 6 hour retention for everybody, phones rotating their addresses every
~15 minutes pin both tables at 64/64 and the counts stop meaning anything. So retention is tiered:

| Candidate | Kept after last sighting |
|---|---|
| elevated, allowed, trusted, or seen at 2+ locations | 6 h |
| single location, 4+ sightings | 1 h |
| single location, fewer sightings | 15 min |

When the table is full anyway, the least recently seen slot is evicted.

### RSSI and distance

Filtered RSSI is an integer EMA (`new = (3*old + sample) / 4`). Distance uses the
log-distance path-loss model:

```text
d = 10 ^ ((-59 - RSSI) / (10 * 2.5))      clamped to 0.5 .. 100 m
```

`-59 dBm at 1 m` is the iBeacon convention, `2.5` is "indoors, stuff in the way".
Real devices differ by +/-10 dB at 1 m and your body absorbs 2.4 GHz nicely, so the
number is often 2x off. Every distance ships with a band ("about 1-4 m") and the UI never
shows the number alone.

---

## 5. Trusted devices: recognising your own phone in disguise

The problem: your own phone is the most persistent BLE device in your life. It will
be `CO-PRESENT` everywhere you go. Allow-listing by MAC does not work, because the
MAC rotates.

The fix is the same trick your phone uses to recognise your earbuds.

```text
 Resolvable Private Address (6 bytes, most significant first)

   +---------------------------+---------------------------+
   | prand (24 bit, top = 01)  | hash (24 bit)             |
   +---------------------------+---------------------------+

   hash = ah(IRK, prand) = AES-128_IRK( 0x00 * 13 || prand )  mod 2^24
```

Whoever holds the device's IRK (Identity Resolving Key) can check any RPA offline in
one AES block. Bluetooth pairing hands over the IRK. So:

1. You open a pairing window (5 min default, cancelable).
2. Passive scanning **stops** (NimBLE cannot build its GATT table while a scan, a GAP
   procedure, is active).
3. The board advertises `AntiStalker-XXXX`, connectable, every 30-60 ms (iOS is slow
   to list devices otherwise).
4. On your phone: Settings > Bluetooth > tap it > type the 6-digit code shown on your
   screen. The board is "display only", MITM protection and LE Secure Connections on.
5. On success, the IRK and identity address go to NVS (max 4 devices), the window
   closes, scanning resumes.

From then on every advert with an RPA is checked against the stored IRKs. A match
gets recorded under the **real identity address**, marked `TRUSTED`, always allowed,
and capped at `NEARBY` forever.

### The byte order trap

Pairing delivers the IRK in **air order** (least significant byte first). AES wants its
key **most significant byte first**. Store it one way, feed it the other, and `ah()`
silently computes garbage, nothing ever matches, and there is no error anywhere.

The code reverses the key before the AES key schedule. Two tests pin it down: spec-style
reference vectors (checked against `openssl enc -aes-128-ecb`) and a real RPA captured
from a bonded Samsung phone. The live capture is the one that caught the bug.

### The "my phone is still a stranger" surprise

After pairing, the phone kept showing up as rotating strangers. Not a bug: phones'
idle "find my" beacons (Apple Continuity, Samsung/Google offline finding) rotate with
**their own vendor schemes**, not the IRK. The IRK-based RPA mostly appears when the
phone **connects**. So the firmware also resolves the address at connection time.
Accessories that advertise connectable RPAs (watches, earbuds) resolve passively in
the scan stream just fine.

---

## 6. Networking

```text
 boot
  |
  V
 start fallback AP "AntiStalker-XXXX" (WPA2, ch 6, 5 dBm, max 2 clients)
  |
  +-- home Wi-Fi saved and enabled? --no--> stay on AP, serve 192.168.4.1
  |
  yes
  V
 AP + STA, try home  --fail--> keep AP, retry home every 30 s
  |
  connected
  V
 AP off, serve on DHCP IP + antistalker-XXXX.local (mDNS)
  |
  home lost --> AP back within ~5 s, keep retrying home
```

- Wi-Fi and BLE share **one** 2.4 GHz radio. Coexistence works, but BLE scan duty is
  50% (80 ms window / 160 ms interval) to leave airtime for Wi-Fi.
- Power: CPU at 80 MHz, AP TX at 5 dBm, BLE starts 1 s after Wi-Fi. Every one of these
  exists because dual-radio startup current spikes are a classic ESP32 `BROWNOUT_RST`.
- Credentials (AP password, home SSID/password) live in the board's NVS, never in the
  app. The API never returns the home password.
- Settings changes apply 1 s after the HTTP reply, so the reply actually reaches the
  client before the network gets ripped out from under it.

### The dashboard

One HTML/CSS/JS blob compiled into flash (`kIndexPage` in `WebPortal.cpp`). No CDN,
no framework, nothing external, so it works on a hotspot with zero internet. Strict CSP
(`connect-src 'self'`, `frame-ancestors 'none'`). Auto-refresh every 3 s, client-side
search and sort.

---

## 7. The companion app

Ionic 9 + Angular 22 + Capacitor 8. Three tabs: Overview, Devices, Setup.

- **CapacitorHttp, not fetch.** The firmware sends no CORS headers (deliberately). In a
  WebView, fetch would be blocked. CapacitorHttp uses the native stack, so CORS does not apply.
- **Every response is validated** field by field (`device-api.service.ts`). Junk from
  the board, or from something pretending to be the board, gives a readable error.
- **Discovery.** If the saved address stops answering, the app probes, in order: the
  saved address, the board's mDNS name, its last known home IP, `192.168.4.1`, and
  `antistalker.local`. The first one to answer wins and is saved. Throttled to once
  every 15 s so a board that is off does not cause a probe storm.
- **GPS sharing** (opt-in) sends a fix at most every 30 s, or sooner after 25 m of
  movement. The phone stores nothing; the board keeps only the latest fix in RAM.
- **Backward compatible** with firmware 0.3.0+: missing fields render as "not available".
- A Leaflet map (OpenStreetMap tiles) for picking the optional home position.

---

## 8. Memory and storage

| What | Where | Survives reboot? |
|---|---|---|
| candidates, history, allow list, location epoch | RAM | no |
| latest phone GPS fix | RAM | no |
| AP password (random, 16 chars) | NVS `antistalker` | yes |
| home Wi-Fi enabled/SSID/password | NVS `antistalker` | yes |
| home position | NVS `antistalker` | yes |
| trusted devices (name, IRK, identity) | NVS `trust` + NimBLE bond store | yes |

`clear` wipes RAM state only. A full flash erase wipes everything, including the AP password.
