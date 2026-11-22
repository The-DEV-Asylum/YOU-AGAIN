# Lab notes

### How it was tested, what broke, what we learned, what we would do differently.

Rule #4 of the Asylum says every project documents what failed. Here is the list of
things that failed. It is not short. That is the point.

---

## How it was tested

### Native unit tests (no board)

```sh
cd firmware
pio test -e native
```

46 Unity tests against the portable library, compiled with `-Wall -Wextra -Wpedantic`:

- real advert layouts for Apple Find My (full and short), Google Find Hub, Samsung SmartTag,
  DULT, Tile, Chipolo, Pebblebee;
- Eddystone vs Find Hub separation, malformed and truncated payloads;
- personal labels: Appearance values, Apple Continuity/Proximity, Fast Pair, Samsung Offline Finding, generic fallback;
- every threshold: `PERSISTENT`, `FOLLOWING`, bursts that must NOT escalate, phones that must not
  escalate from time alone, one sighting per location that must not count;
- identity: payload IDs surviving MAC rotation, address type separating MAC identities;
- owner-state handling, allow lists, RSSI filtering, tiered retention;
- trust: RPA shape, `ah()` reference vectors, **a live captured RPA from a bonded phone**,
  rotation, name capture, table management, address parsing;
- distance estimate monotonicity and bands.

They also run in CI on every firmware change.

### App tests

```sh
cd app
npm run test:ci
```

41 Vitest tests: models and helpers, API response validation (including junk responses and bad
endpoints), and the monitor service (polling, discovery, address adoption).

### Hardware smoke tests (ESP32-S3 rev 0.2, N16R8)

- 16 MB flash and ~8 MB PSRAM detected at boot.
- Continuous BLE scanning with real Find Hub, Apple Continuity, audio, Fast Pair and unclassified devices.
- WPA2 AP visible; random password persisted across resets and AP restarts.
- Home Wi-Fi saved via `POST /api/wifi`: AP stopped after DHCP, HTTP and BLE kept working.
- Disabling home Wi-Fi from the Android app restored the AP, kept the credentials, and the
  setting survived a reset.
- Deliberately wrong home SSID: AP stayed up across repeated 30 s retries; fixing the
  credentials reconnected and stopped the AP again.
- BLE stayed active with Wi-Fi on and through Wi-Fi off/on, with **zero queue drops** observed.
- Serial commands `list`, `devices`, `help`, `moved`, `stats`, `wifi`, allow/unallow, `locate` exercised.
- Trusted-device pairing with a real Samsung phone; its live RPA became a test vector.
- Dashboard JavaScript: syntax checked and tested against a mocked DOM (search, empty
  results, refresh persistence, no network requests while searching).

### Not tested yet

- **The end-to-end `FOLLOWING` alert in the field**: a real tag travelling with someone
  across two genuinely different places. Unit tests cover the logic; the field trip has
  not happened. Until it has, "it works" means "every part works, the whole has not been
  taken on the bus".
- The CI pipeline (`release.yml`: tests -> builds -> semver tag) was written for this release and has not run on GitHub yet. The commands
  inside them were run locally (native tests, ESP32 build, image merge, app build and tests).
- iOS on a real iPhone via the unsigned IPA path.

---

## What broke, and what it taught us

### 1. The byte order of the IRK (the big one)

**Symptom:** the resolver computed hashes that did not match the rotating address captured from a
real bonded phone. No error, no crash, just a silent mismatch.

**Cause:** Bluetooth distributes the IRK least significant byte first. AES takes its key most
significant byte first. Same 16 bytes, wrong way round, completely different cipher.

**Fix:** reverse the key before the AES key schedule. Pinned with a real captured RPA from a
bonded phone, which is now a test that must never be deleted.

**Lesson:** crypto bugs do not throw exceptions. They just quietly return wrong numbers.
Test against real captured data, not only against your own understanding of the spec.

### 2. "I paired my phone and it is still a stranger"

**Symptom:** after a successful pairing, the phone still showed up as several unknown devices.

**Cause:** phones' idle beacons (Find My, Continuity, Samsung/Google offline finding) rotate
using their own vendor schemes, not the Bluetooth IRK. The IRK-based address mostly shows up
when the phone connects.

**Fix:** also resolve the peer address on connection. Document that idle beacons will stay
strangers, by design.

**Lesson:** a spec tells you what a device *may* do. Field captures tell you what it *does*.

### 3. Tables stuck at 64/64

**Symptom:** with one 6 hour retention for everybody, both candidate tables fill up and stay
full, so the counts stop meaning anything.

**Cause:** phones rotate private addresses every ~15 min, so every phone becomes a new candidate
several times an hour.

**Fix:** tiered retention (15 min / 1 h / 6 h, see ENGINEERING.md). Evidence is kept, noise expires.
The app shows "active now" (seen within 2 min) instead of raw table sizes.

**Lesson:** on a bounded table, the eviction policy IS the product.

### 4. Two radios, one cheap USB cable

**Risk:** an ESP32 starting Wi-Fi and BLE at the same time at full power pulls a nasty current
spike, and a weak supply answers with `BROWNOUT_RST`.

**Mitigations:** CPU at 80 MHz, AP TX power 5 dBm, BLE scan duty 50%, BLE started 1 s after Wi-Fi.
If you still see repeated `BROWNOUT_RST` in the log, get a better cable or power supply.

**Lesson:** "the code is fine" and "the board keeps rebooting" can both be true at the same time.

### 5. CORS vs a microcontroller

**Problem:** the firmware sends no CORS headers, so plain `fetch()` calls from a WebView or a
browser to the board get blocked.

**Choice:** we did not add CORS headers to the firmware (it would make cross-origin
requests from random web pages easier, which the `X-AntiStalker` header is there to stop).

**Fix:** use CapacitorHttp in the app, which goes through the native HTTP stack.
Downside: the browser preview of the app (`npm start`) cannot really talk to the board.

### 6. iOS takes its time

**Symptom:** the pairing advert took ages to appear in iOS Bluetooth settings.

**Fix:** advertise every 30-60 ms, legacy adverts, only while the window is open.

### 7. Phones that pair but do not bond

**Symptom:** "paired", but no IRK.

**Cause:** some phones tap-pair transiently without distributing keys.

**Fix:** ignore pairings without an IRK and keep the window open, so the user can simply retry.

### 8. Indoor GPS is a liar

**Risk:** phone GPS indoors drifts by hundreds of meters, which would auto-advance location epochs
while you sit on the sofa, which would turn your flatmate into a "follower".

**Fix:** auto-epoch only after 150 m of movement, and only with fixes that report 100 m accuracy
or better. Accuracy 0 (unknown) never counts.

### 9. Documentation drift (found while preparing this release)

- The old firmware README said "34 native tests". There are 46.
- The old app README documented `accuracyM` for `POST /api/home`. The firmware and app both use `accuracy`.

**Lesson:** docs rot faster than code. That is why the numbers in these docs were re-counted
from the source, not copied from the previous README.

### 10. Known debt

- `npm run lint` reports 11 errors, all `@angular-eslint/template/eqeqeq` on `!= null` checks in
  templates. Those checks deliberately catch both `null` and `undefined`. Either rewrite them or
  relax the rule; CI does not run lint until one of the two happens.
- UI strings (dashboard and app) still use a few typographic characters (middle dots, degree
  signs, arrows, curly quotes). Comments and docs are plain ASCII.

---

## What we would do differently

- **API authentication.** A per-board token printed once on serial and stored in the app, required
  on every request. Right now anyone on the same network is "the owner" (see THREAT_MODEL.md).
- **Persist the allow list.** It lives in RAM and dies with every reboot, which is annoying.
  (Trusted devices survive; plain allow entries do not.)
- **Protect evidence from eviction.** Under a deliberate MAC flood, elevated candidates can be evicted.
  Reserve some slots for them.
- **Flash encryption + NVS encryption** for anything that leaves the house.
- **Active features, carefully.** DULT defines a way to make a tag play a sound. That would turn
  "probably in your bag" into "found it". It would also mean giving up being purely passive, so it
  needs thought.
- **A field test protocol** with real tags, a second person and a fixed route, repeated, with
  logged false positive / false negative rates. That is the next experiment.
