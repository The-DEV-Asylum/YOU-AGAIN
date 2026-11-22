/*
 * ===========================================================================
 *  AntiStalker :: PersonalDeviceClassifier.cpp
 *  Same AD-structure walk as TrackerClassifier, different zoo.
 * ===========================================================================
 */

#include "PersonalDeviceClassifier.h"

namespace antistalker {
namespace {

constexpr uint8_t kAdServiceUuid16Incomplete = 0x02;
constexpr uint8_t kAdServiceUuid16Complete = 0x03;
constexpr uint8_t kAdServiceData16 = 0x16;
constexpr uint8_t kAdAppearance = 0x19;
constexpr uint8_t kAdManufacturerData = 0xFF;
constexpr uint16_t kCompanyApple = 0x004C;
constexpr uint16_t kUuidFastPair = 0xFE2C;
constexpr uint16_t kUuidSamsungFindMyMobile = 0xFD69;

uint16_t readLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

int personalPriority(TrackerFamily family) {
    switch (family) {
        case TrackerFamily::Phone:
        case TrackerFamily::Watch:
        case TrackerFamily::AudioDevice:
            return 90;
        case TrackerFamily::AppleProximityAccessory:
            return 80;
        case TrackerFamily::FastPairAccessory:
            return 70;
        case TrackerFamily::AppleContinuityDevice:
        case TrackerFamily::SamsungMobileDevice:
            return 60;
        case TrackerFamily::GenericBle:
            return 1;
        default:
            return 0;
    }
}

TrackerMatch classifyAppearance(uint16_t appearance) {
    TrackerMatch match;
    // Appearance = 10-bit category + 6-bit subcategory (Bluetooth Assigned
    // Numbers). We mostly care about the category.
    const uint16_t category = appearance >> 6;
    if (category == 0x001) {
        match.family = TrackerFamily::Phone;
    } else if (category == 0x003 || appearance == 0x0086) {
        // Generic Watch category plus the watch-size wearable computer value.
        match.family = TrackerFamily::Watch;
    } else if (category == 0x021 || category == 0x022 || category == 0x025) {
        // Audio Sink, Audio Source, and Wearable Audio Device categories.
        match.family = TrackerFamily::AudioDevice;
    }
    return match;
}

TrackerMatch classifyApple(const uint8_t* value, size_t valueLength) {
    TrackerMatch best;
    if (valueLength < 4 || readLe16(value) != kCompanyApple) {
        return best;
    }

    // Apple Continuity = a pile of type-length-value messages glued together
    // after the company ID. Walk them, bounds-check every one.
    size_t offset = 2;
    while (offset + 2 <= valueLength) {
        const uint8_t type = value[offset];
        const uint8_t messageLength = value[offset + 1];
        offset += 2;
        if (messageLength > valueLength - offset) {
            break;
        }

        TrackerMatch candidate;
        if (type == 0x07) {
            // Proximity Pairing: usually AirPods/Beats. We do not guess which
            // model; "Audio device" is as far as the bytes let us go.
            candidate.family = (messageLength == 0x19 && value[offset] == 0x01)
                                   ? TrackerFamily::AudioDevice
                                   : TrackerFamily::AppleProximityAccessory;
        } else if ((type == 0x10 && messageLength == 5) ||
                   (type == 0x0C && messageLength == 0x0E)) {
            // Nearby Info / Handoff. iPhones send these, but so do iPads, Macs
            // and Watches, so the label says "Continuity device", not "phone".
            candidate.family = TrackerFamily::AppleContinuityDevice;
        }
        if (personalPriority(candidate.family) > personalPriority(best.family)) {
            best = candidate;
        }
        offset += messageLength;
    }
    return best;
}

} // namespace

TrackerMatch PersonalDeviceClassifier::classify(const uint8_t* payload, size_t length) {
    TrackerMatch best;
    if (payload == nullptr) {
        return best;
    }
    best.family = TrackerFamily::GenericBle;

    size_t offset = 0;
    while (offset < length) {
        const uint8_t adLength = payload[offset];
        if (adLength == 0) {
            break;
        }
        if (adLength > length - offset - 1) {
            break;
        }

        const uint8_t adType = payload[offset + 1];
        const uint8_t* value = payload + offset + 2;
        const size_t valueLength = adLength - 1;
        TrackerMatch candidate;

        if (adType == kAdAppearance && valueLength >= 2) {
            candidate = classifyAppearance(readLe16(value));
        } else if (adType == kAdManufacturerData) {
            candidate = classifyApple(value, valueLength);
        } else if (adType == kAdServiceData16 && valueLength >= 2) {
            const uint16_t uuid = readLe16(value);
            if (uuid == kUuidFastPair) {
                candidate.family = TrackerFamily::FastPairAccessory;
            } else if (uuid == kUuidSamsungFindMyMobile &&
                       (valueLength == 16 || valueLength == 22)) {
                // Only the 14/20-byte Offline Finding payloads that were
                // actually studied. Not a "Samsung phone detector", and not
                // proof that anything is lost.
                candidate.family = TrackerFamily::SamsungMobileDevice;
            }
        } else if ((adType == kAdServiceUuid16Incomplete || adType == kAdServiceUuid16Complete) &&
                   valueLength >= 2 && valueLength % 2 == 0) {
            for (size_t i = 0; i < valueLength; i += 2) {
                const uint16_t uuid = readLe16(value + i);
                TrackerMatch listed;
                if (uuid == kUuidFastPair) {
                    listed.family = TrackerFamily::FastPairAccessory;
                }
                if (personalPriority(listed.family) > personalPriority(candidate.family)) {
                    candidate = listed;
                }
            }
        }

        if (personalPriority(candidate.family) > personalPriority(best.family)) {
            best = candidate;
        }
        offset += static_cast<size_t>(adLength) + 1;
    }

    // No payload fingerprinting for personal devices, on purpose: their
    // payloads change every few seconds (battery, state, counters) and
    // hashing them would spawn a "new device" per advert. MAC + address type
    // it is. Yes, that means rotating phones get split. See the README.
    best.hasIdentityPayload = false;
    return best;
}

} // namespace antistalker
