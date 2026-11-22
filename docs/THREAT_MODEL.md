# Threat model

### Rule #5 of the Asylum: security is still security.

Yes, even for a 10 euro board on a keychain. Especially for one that is supposed to
help people who may be in an unsafe situation. This page is written with a straight face.

---

## What we protect, and from whom

**Assets**

1. The user's **safety**: the verdicts must not lull anyone into false security, or
   cause a panic or a confrontation over nothing.
2. The user's **privacy**: where they go, what they own, which devices are theirs.
3. The **trusted-device keys** (IRKs). Whoever has them can recognise your phone's
   rotating addresses, which is the same capability we are trying to detect.
4. The **network credentials** stored on the board (AP password, home Wi-Fi password).

**Adversaries**

| Who | Wants | Capability |
|---|---|---|
| A: the stalker | stay undetected | Owns a commercial tag, maybe a modified one. Can read this repo. |
| B: a nearby nerd | mess with you | Laptop/phone that can broadcast arbitrary BLE adverts. |
| C: someone on the same Wi-Fi | read your data | Joined your home network, or got the AP password. |
| D: someone with the board in hand | everything | USB cable and patience. |

---

## Threats and what we do about them

### A1. Tracker avoids detection

| Trick | Effect on us | Mitigation | Residual risk |
|---|---|---|---|
| Uses a tag protocol we do not know | Invisible as a tracker | Still tracked as "Unclassified BLE" by the device engine, can reach `CO-PRESENT` by MAC | MAC rotation defeats that too |
| Rotates MAC **and** payload ID together | Candidate splits, never escalates | Payload-based identity survives MAC-only rotation | Real false negative, by design of the tag |
| Fakes "near owner" (DULT bit, Samsung connected state) | Stays `NEARBY` | None possible: the bit is unauthenticated. History is kept so it escalates if the bit ever flips | **Known gap.** A modified tag can lie its way out |
| Bluetooth Classic, cellular, GPS-only, or powered off | Invisible | Documented loudly | Out of reach of an ESP32-S3, physics |
| Stays silent, transmits rarely | Few sightings, slow escalation | Thresholds are counts, not rates; 6 h retention once something is interesting | Slow tags escalate slowly |

**Bottom line:** this is a screening tool against commercial trackers behaving per
spec. A determined adversary with custom hardware beats it. Platform tracker alerts
(iOS/Android) and a physical inspection are still the real answer.

### B. Spoofed and malicious adverts

- **Fake devices / alert flooding.** Anyone can broadcast a fake AirTag. We cannot
  authenticate adverts, nobody can. Mitigations: escalation needs repeated sightings at
  *several user-confirmed places*, which a random prankster at one location cannot fake
  easily. Every message says "risk signal, not proof".
- **Table exhaustion.** Flooding thousands of fake MACs evicts real candidates. The table
  is bounded (64) and evicts least-recently-seen, so the board survives, but a dedicated
  flood can push a real tag out. Elevated candidates are not protected from eviction
  under flood. Known limitation.
- **Parser attacks.** Every AD length is validated before reading, nested TLVs are bounds
  checked, payloads are capped at 255 bytes, there is no heap in the parser. Malformed
  payload tests exist.
- **Terminal injection.** BLE names are reduced to printable ASCII before printing.
  The dashboard builds rows with `textContent`, not `innerHTML`, for device data.
- **Queue flooding.** A fast advert flood fills the queue, drops are counted
  (`dropped` in stats) and the main loop stays responsive (max 16 events per pass).

### B2. Stranger tries to become "trusted"

A trusted device can never alert, so becoming trusted is the jackpot for a stalker.

- Pairing only happens inside a window **you** open, and it closes by itself (5 min default, 15 min max).
- MITM-protected passkey pairing, LE Secure Connections. The 6-digit code comes from the
  hardware RNG and is shown only on your screen. No code, no bond.
- Pairings that share no IRK are ignored.
- Residual: someone who can read your screen during the window, or who can already open
  the pairing window (see C). Also: a trusted device proves "this holds the key you
  paired", never "this is in your hands right now". If your trusted phone is stolen and
  planted on you... ok, that is a stretch, but it is written down now.

### C. Someone on the same network

This is the weakest part, and we know it.

- **There is no user authentication on the HTTP API.** Anyone on the active network can
  `GET /api/status`: candidate tables, trusted device names, phone GPS fix, home position.
- **Anyone on the network can also POST actions** (allow, move, pair, change Wi-Fi). The
  `X-AntiStalker: 1` header is a **CSRF barrier**, not authentication: a malicious web page
  in your browser cannot send it cross-origin without a CORS preflight, and the board
  answers none. A tool like `curl` on the same LAN sends it trivially.
- **Plain HTTP.** No TLS on the board. Traffic is protected only by WPA2 on the hotspot,
  or by whatever your home network does.
- **The passkey is in `/api/status`** while a pairing window is open. Combined with the
  above: someone on your Wi-Fi could open a window and pair themselves. This is the
  main reason the docs say "only use trusted home networks".

Mitigations in place: the hotspot is WPA2 with a random 16-character password (~93 bits),
TX power is 5 dBm (short range), max 2 clients, the home password is write-only via the API,
IRKs never leave over HTTP, and nothing is ever sent beyond the local network.

What we would add next (see [LAB_NOTES.md](LAB_NOTES.md)): a per-board API token shown
once on serial and stored in the app, required on every request.

### D. Physical access

- The serial console has no login. `trustdump` prints the IRKs in hex, `wifi` prints the AP
  password. Deliberate: physical access to an unencrypted ESP32 already means game over.
- NVS is not encrypted (no flash encryption enabled). Someone who steals the board can read
  the AP password, home Wi-Fi password, home position and IRKs.
- If you lose the board: change your home Wi-Fi password and remove the board from your
  phone's Bluetooth settings.

---

## Privacy

| Data | Stored | Leaves the board? |
|---|---|---|
| Nearby device observations | RAM only, lost on reboot | Only to clients on the local network |
| Where you went | **Never.** Only an epoch counter | No |
| Phone GPS fix (opt-in) | RAM, latest fix only | Only to local clients |
| Home position (optional) | NVS | Only to local clients |
| IRKs of your devices | NVS | Serial console only |

The app stores only its connection settings and the board's last known address. No
analytics, no cloud, no crash reporting. The only external requests the app makes are
OpenStreetMap tiles, when you open the home-position map.

Note that the board, by its nature, collects MAC addresses of everyone around you. They stay
in RAM and expire within minutes to hours. Do not add a "log everything to SD card" feature
without thinking hard about what you would then be carrying around.

---

## False positives and false negatives

| | Main causes |
|---|---|
| **False positive** (alert, but harmless) | your own untrusted devices; a travel companion; public transport regulars; shared/static devices; spoofing |
| **False negative** (missed tracker) | MAC + payload rotation; unknown protocols; Classic/cellular trackers; faked near-owner bit; coexistence packet loss; never confirming a location change |

The design leans towards false negatives over false positives on purpose. Constant false
alarms train people to ignore alarms, and an accusation based on radio noise can hurt
someone who did nothing.
