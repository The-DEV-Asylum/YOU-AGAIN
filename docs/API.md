# HTTP API

Firmware **0.5.0**. Served on port 80, on `http://192.168.4.1` (fallback hotspot) or on the
board's home IP / `http://antistalker-XXXX.local` (home Wi-Fi).

Ground rules:

- JSON in, JSON out. Errors look like `{"error":"..."}` with a 4xx/5xx status.
- Every `POST` needs the header **`X-AntiStalker: 1`**, or you get `403 forbidden`.
  It is a CSRF barrier, not a password (see [THREAT_MODEL.md](THREAT_MODEL.md)).
- No CORS headers. Use the companion app (native HTTP), `curl`, or the built-in dashboard.
- No authentication. Anyone on the network can call this. Plan accordingly.

```sh
# read everything
curl http://192.168.4.1/api/status

# "I am somewhere else now"
curl -X POST -H 'X-AntiStalker: 1' http://192.168.4.1/api/moved
```

---

## Endpoints

| Method | Path | Body | Does |
|---|---|---|---|
| GET | `/` | | The dashboard (HTML, everything inline) |
| GET | `/api/status` | | Full status, see below |
| POST | `/api/moved` | | New location epoch (and re-anchors GPS auto-epochs) |
| POST | `/api/location` | `{"latitude":n,"longitude":n,"accuracy":n?}` | Phone GPS fix. May auto-advance the epoch (>= 150 m, accuracy 1-100 m) |
| POST | `/api/allow?id=<8 hex>&allowed=<0\|1>` | | Allow/unallow a candidate (RAM only) |
| POST | `/api/wifi` | `{"enabled":bool,"ssid":s?,"password":s?}` | Save/enable/disable home Wi-Fi. SSID 1-32 bytes, password empty or 8-63. ssid and password go together. Applied ~1 s later |
| POST | `/api/trust/pair` | `{"seconds":n}` (optional, default 300, max 900) | Open the pairing window. Returns `passkey`, `name`, `expiresS` |
| POST | `/api/trust/cancel` | | Close the pairing window |
| POST | `/api/trust/name` | `{"address":"AA:BB:..","name":s}` | Rename a trusted device (1-24 bytes) |
| POST | `/api/trust/remove` | `{"address":"AA:BB:.."}` | Delete a trusted device and its bond |
| POST | `/api/home` | `{"latitude":n,"longitude":n,"accuracy":n?,"source":"phone"\|"map"\|"manual"}` | Set home position (NVS) |
| POST | `/api/home/remove` | | Forget the home position |

`accuracy` is in meters, 0-100000. Coordinates are range-checked.

---

## `GET /api/status`

Abridged, real field names:

```json
{
  "version": "0.5.0",
  "ssid": "AntiStalker-1A2B",
  "mdns": "antistalker-1a2b.local",
  "uptime": 3600,
  "scanning": true,
  "received": 120345, "dropped": 0,
  "trackerPackets": 812, "devicePackets": 95000,
  "devicesEnabled": true,
  "locationEpoch": 2,
  "alerts": 0,
  "wifiClients": 1,
  "homeWifi": { "enabled": false, "configured": false, "connected": false, "ssid": "", "ip": "" },

  "chipModel": "ESP32-S3",
  "freeHeap": 180000, "totalHeap": 320000,
  "freePsram": 8300000, "totalPsram": 8380000,
  "sketchSize": 1011136, "flashFree": 5200000,
  "maxCandidates": 64,
  "temperatureC": 41.5,
  "battery": { "available": false, "millivolts": null, "percent": null },

  "phoneLocation": { "valid": true, "latitude": 0, "longitude": 0, "accuracyM": 12, "ageS": 4, "autoEpochDistanceM": 150 },
  "home": { "valid": false, "latitude": null, "longitude": null, "accuracyM": null, "ageS": null, "source": "", "phoneDistanceM": null },
  "trust": {
    "pairing": false,
    "devices": [ { "name": "My phone", "identity": "54:EB:E9:77:8A:EA", "addressType": 0, "resolved": 42 } ]
  },

  "trackers": [
    {
      "id": "1F2E3D4C", "type": "Google Find Hub", "state": "NEARBY",
      "mac": "C0:11:22:33:44:55", "addressType": 1,
      "rssi": -71, "peak": -64, "sightings": 18, "minutes": 5, "locations": 1,
      "owner": "unknown", "lastSeen": 3, "allowed": false, "trusted": false,
      "distanceM": 3, "distanceBand": "about 1-4 m"
    }
  ],
  "devices": [ "... same shape ..." ]
}
```

Notes:

- `state` is always `NEARBY`, `PERSISTENT` or `FOLLOWING`. For entries in `devices`, the UIs
  display `FOLLOWING` as `CO-PRESENT`.
- `owner` is `near`, `separated` or `unknown`.
- `network` (DULT network ID) appears only on DULT trackers; `trustedName` only on trusted entries.
- While pairing is open, `trust` also has `name`, `passkey` (6-digit string, keeps leading zeros)
  and `expiresS`.
- `temperatureC` can be `null`. `battery` values are `null` unless the battery build flags are set.
- The home Wi-Fi password is never returned. IRKs are never returned.

Serial-only features with no HTTP route: `locate`, `verbose`, `clear`, `devices on|off`, `trustdump`.
