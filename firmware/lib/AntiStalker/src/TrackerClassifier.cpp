/*
 * ===========================================================================
 *  AntiStalker :: TrackerClassifier.cpp
 *  The tracker zoo. One classifyXxx() per vendor, then a priority contest.
 * ===========================================================================
 *
 *  Sources for every magic number below are listed in the README
 *  ("Protocol references"). If a constant looks random, it is not: it came
 *  out of a spec, an Internet-Draft, or somebody's reverse-engineering paper.
 * ===========================================================================
 */

#include "TrackerClassifier.h"

namespace antistalker {
namespace {

constexpr uint8_t kAdServiceUuid16Incomplete = 0x02;
constexpr uint8_t kAdServiceUuid16Complete = 0x03;
constexpr uint8_t kAdServiceData16 = 0x16;
constexpr uint8_t kAdManufacturerData = 0xFF;
constexpr uint16_t kCompanyApple = 0x004C;
constexpr uint16_t kUuidDult = 0xFCB2;
constexpr uint16_t kUuidGoogleFindHub = 0xFEAA;
constexpr uint16_t kUuidSamsungSmartTag = 0xFD5A;
constexpr uint16_t kUuidTile = 0xFEED;
constexpr uint16_t kUuidChipolo = 0xFE33;
constexpr uint16_t kUuidPebblebee = 0xFA25;

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

// FNV-1a, 32 bit. Not crypto, does not need to be: it only turns a payload
// into a compact candidate key. Collisions just merge two candidates.
uint32_t fnv1a(const uint8_t* data, size_t length, uint32_t hash = 2166136261UL) {
    for (size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= 16777619UL;
    }
    return hash;
}

TrackerMatch classifyApple(const uint8_t* value, size_t valueLength) {
    TrackerMatch match;
    if (valueLength < 6 || value[2] != 0x12 || valueLength < 4U + value[3]) {
        return match;
    }

    if (value[3] == 0x19) {
        // Full Offline Finding frame: carries (part of) the rotating public key.
        // Could be an AirTag, could be any other Find My accessory. From
        // passive data we honestly cannot tell which, so we do not pretend.
        match.family = TrackerFamily::AppleFindMy;
        // Hash from byte 5 on: skip company ID, type, length and the status
        // byte, otherwise a battery-level change would "create" a new tag.
        match.identityHint = fnv1a(value + 5, valueLength - 5);
        match.hasIdentityPayload = valueLength > 5;
    } else if (value[3] == 0x02) {
        // Short Offline Finding frame, seen on AirTags near their owner.
        // Device class 0 is a phone/tablet/Mac: every iPhone in the bus would
        // become a "tracker". Excluded.
        const uint8_t deviceClass = (value[4] >> 4) & 0x03U;
        if (deviceClass != 0) {
            match.family = TrackerFamily::AppleFindMy;
        }
    }
    return match;
}

TrackerMatch classifyDult(const uint8_t* value, size_t valueLength) {
    TrackerMatch match;
    if (valueLength < 4) {
        return match;
    }

    // DULT (the IETF "Detecting Unwanted Location Trackers" draft) service
    // data: UUID, network ID, near-owner/reserved byte, optional vendor data.
    // The near-owner bit is NOT hashed: a tag walking away from its owner
    // must stay the same candidate, that is the whole point.
    match.family = TrackerFamily::Dult;
    match.networkId = value[2];
    match.nearOwnerKnown = true;
    match.nearOwner = (value[3] & 0x01U) != 0;

    if (valueLength > 4) {
        const uint32_t hash = fnv1a(&match.networkId, 1);
        match.identityHint = fnv1a(value + 4, valueLength - 4, hash);
        match.hasIdentityPayload = true;
    }
    return match;
}

TrackerMatch classifyGoogle(const uint8_t* value, size_t valueLength) {
    TrackerMatch match;
    // FEAA is shared with plain old Eddystone beacons (museum, shop, that
    // one conference badge). A Find Hub frame needs type 0x40/0x41 AND an
    // exact 20 or 32 byte EID (+ optional 1-byte hashed flags). Anything
    // else is a beacon minding its own business.
    if (valueLength < 23 || (value[2] != 0x40 && value[2] != 0x41) ||
        (valueLength != 23 && valueLength != 24 && valueLength != 35 && valueLength != 36)) {
        return match;
    }

    const size_t eidLength = (valueLength == 23 || valueLength == 24) ? 20 : 32;
    match.family = TrackerFamily::GoogleFindHub;
    match.identityHint = fnv1a(value + 3, eidLength);
    match.hasIdentityPayload = true;
    return match;
}

TrackerMatch classifySamsung(const uint8_t* value, size_t valueLength) {
    TrackerMatch match;
    // Registered SmartTag/SmartTag2 offline-finding frame. 13 bytes of
    // service payload get us through the flags byte; some tags send 20.
    if (valueLength < 15 || (value[2] & 0xF8U) != 0x10U) {
        return match;
    }

    match.family = TrackerFamily::SamsungSmartTag;
    match.identityHint = fnv1a(value + 6, 8); // Current 8-byte privacy ID.
    match.hasIdentityPayload = true;

    const uint8_t state = value[2] & 0x07U;
    if (state >= 1 && state <= 6) {
        match.nearOwnerKnown = true;
        match.nearOwner = state >= 4; // States 1-3 offline; 4-6 connected (to owner).
    }
    return match;
}

TrackerMatch classifyServiceData(uint16_t uuid, const uint8_t* value, size_t valueLength) {
    TrackerMatch match;
    switch (uuid) {
        case kUuidDult:
            return classifyDult(value, valueLength);
        case kUuidGoogleFindHub:
            return classifyGoogle(value, valueLength);
        case kUuidSamsungSmartTag:
            return classifySamsung(value, valueLength);
        case kUuidTile:
            match.family = TrackerFamily::Tile;
            break;
        case kUuidChipolo:
            match.family = TrackerFamily::Chipolo;
            break;
        default:
            return match;
    }

    if (valueLength > 2) {
        match.identityHint = fnv1a(value + 2, valueLength - 2);
        match.hasIdentityPayload = true;
    }
    return match;
}

TrackerMatch classifyServiceUuid(uint16_t uuid) {
    TrackerMatch match;
    switch (uuid) {
        case kUuidTile:
            match.family = TrackerFamily::Tile;
            break;
        case kUuidChipolo:
            match.family = TrackerFamily::Chipolo;
            break;
        case kUuidPebblebee:
            match.family = TrackerFamily::Pebblebee;
            break;
        default:
            break;
    }
    return match;
}

int matchPriority(TrackerFamily family) {
    switch (family) {
        case TrackerFamily::Dult:
            return 100; // Standard near-owner state = the most useful info we can get.
        case TrackerFamily::GoogleFindHub:
        case TrackerFamily::SamsungSmartTag:
            return 90;
        case TrackerFamily::AppleFindMy:
            return 80;
        case TrackerFamily::Tile:
            return 70;
        case TrackerFamily::Chipolo:
        case TrackerFamily::Pebblebee:
            return 60;
        case TrackerFamily::None:
        case TrackerFamily::Phone:
        case TrackerFamily::Watch:
        case TrackerFamily::AudioDevice:
        case TrackerFamily::AppleContinuityDevice:
        case TrackerFamily::AppleProximityAccessory:
        case TrackerFamily::FastPairAccessory:
        case TrackerFamily::SamsungMobileDevice:
        case TrackerFamily::GenericBle:
        default:
            return 0;
    }
}

} // namespace

// Walks every AD structure, classifies each, keeps the best. "Best" = highest
// family priority, or same family but now with an identity payload.
TrackerMatch TrackerClassifier::classify(const uint8_t* payload, size_t length) {
    TrackerMatch bestMatch;
    if (payload == nullptr) {
        return bestMatch;
    }

    size_t offset = 0;
    while (offset < length) {
        const uint8_t adLength = payload[offset];
        if (adLength == 0) {
            break;
        }

        // adLength includes the AD type but excludes its own length byte.
        if (adLength > length - offset - 1) {
            break; // Length points past the buffer: malformed, stop reading. Safely.
        }

        const uint8_t adType = payload[offset + 1];
        const uint8_t* value = payload + offset + 2;
        const size_t valueLength = adLength - 1;
        TrackerMatch candidate;

        if (adType == kAdManufacturerData && valueLength >= 4) {
            if (readLe16(value) == kCompanyApple) {
                candidate = classifyApple(value, valueLength);
            }
        } else if (adType == kAdServiceData16 && valueLength >= 2) {
            candidate = classifyServiceData(readLe16(value), value, valueLength);
        } else if ((adType == kAdServiceUuid16Incomplete || adType == kAdServiceUuid16Complete) &&
                   valueLength >= 2 && valueLength % 2 == 0) {
            // A device can list several UUIDs. Pick the tracker one, ignore
            // the "I am also a heart-rate monitor" one next to it.
            for (size_t i = 0; i < valueLength; i += 2) {
                TrackerMatch listed = classifyServiceUuid(readLe16(value + i));
                if (matchPriority(listed.family) > matchPriority(candidate.family)) {
                    candidate = listed;
                }
            }
        }

        const bool betterFamily = matchPriority(candidate.family) > matchPriority(bestMatch.family);
        const bool betterIdentity = candidate.family == bestMatch.family &&
                                    candidate.hasIdentityPayload && !bestMatch.hasIdentityPayload;
        if (candidate.family != TrackerFamily::None && (betterFamily || betterIdentity)) {
            bestMatch = candidate;
        }
        offset += static_cast<size_t>(adLength) + 1;
    }

    return bestMatch;
}

const char* trackerFamilyName(TrackerFamily family) {
    switch (family) {
        case TrackerFamily::AppleFindMy:
            return "Apple Find My";
        case TrackerFamily::GoogleFindHub:
            return "Google Find Hub";
        case TrackerFamily::SamsungSmartTag:
            return "Samsung SmartTag";
        case TrackerFamily::Tile:
            return "Tile";
        case TrackerFamily::Chipolo:
            return "Chipolo";
        case TrackerFamily::Pebblebee:
            return "Pebblebee";
        case TrackerFamily::Dult:
            return "DULT tracker";
        case TrackerFamily::Phone:
            return "Phone (appearance)";
        case TrackerFamily::Watch:
            return "Watch (appearance)";
        case TrackerFamily::AudioDevice:
            return "Audio device";
        case TrackerFamily::AppleContinuityDevice:
            return "Apple Continuity device";
        case TrackerFamily::AppleProximityAccessory:
            return "Apple proximity accessory";
        case TrackerFamily::FastPairAccessory:
            return "Fast Pair accessory";
        case TrackerFamily::SamsungMobileDevice:
            return "Samsung Offline Finding";
        case TrackerFamily::GenericBle:
            return "Unclassified BLE";
        case TrackerFamily::None:
        default:
            return "other BLE";
    }
}

// Relies on enum order: everything from Phone onward is a personal device.
bool isPersonalDeviceFamily(TrackerFamily family) {
    return static_cast<uint8_t>(family) >= static_cast<uint8_t>(TrackerFamily::Phone);
}

} // namespace antistalker
