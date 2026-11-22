/*
 * ===========================================================================
 *  AntiStalker :: TrackerClassifier.h
 *  "Is that an AirTag in your pocket or are you just happy to see me?"
 * ===========================================================================
 *
 *  Pure, platform-independent C++11. No Arduino, no NimBLE, no heap. That is
 *  what lets the whole thing run in the native unit tests on a laptop.
 *
 *  Input: raw BLE AD structures, i.e. a list of
 *
 *      [length][AD type][value ...][length][AD type][value ...] ...
 *
 *  Output: which tracker family (if any), plus an "identity hint" hashed from
 *  the protocol-specific payload, so an AirTag that rotates its MAC is still
 *  the same candidate. Every length is validated BEFORE it is read. A
 *  malformed advert gets "None", never a buffer over-read.
 * ===========================================================================
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace antistalker {

enum class TrackerFamily : uint8_t {
    None = 0,
    AppleFindMy,
    GoogleFindHub,
    SamsungSmartTag,
    Tile,
    Chipolo,
    Pebblebee,
    Dult,
    // ---- everything below is NOT a tracker. These are the ordinary
    // devices used by the co-presence detector. isPersonalDeviceFamily()
    // relies on this ordering, so add new trackers ABOVE this line.
    Phone,
    Watch,
    AudioDevice,
    AppleContinuityDevice,
    AppleProximityAccessory,
    FastPairAccessory,
    SamsungMobileDevice,
    GenericBle,
};

struct TrackerMatch {
    TrackerFamily family = TrackerFamily::None;
    // FNV-1a hash of the payload bytes that identify one physical tag.
    // Only meaningful when hasIdentityPayload is true.
    uint32_t identityHint = 0;
    bool hasIdentityPayload = false;
    // Some protocols (DULT, Samsung) say whether the tag is near its owner.
    // "near owner" = probably just somebody's keys, do not escalate.
    bool nearOwnerKnown = false;
    bool nearOwner = false;
    uint8_t networkId = 0; // DULT only: which finding network.
};

class TrackerClassifier {
  public:
    // Parses BLE AD structures, not a complete BLE PDU. The input includes each
    // AD length byte and manufacturer data includes its two-byte company ID.
    static TrackerMatch classify(const uint8_t* payload, size_t length);
};

const char* trackerFamilyName(TrackerFamily family);
bool isPersonalDeviceFamily(TrackerFamily family);

} // namespace antistalker
