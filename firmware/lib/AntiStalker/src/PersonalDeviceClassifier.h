/*
 * ===========================================================================
 *  AntiStalker :: PersonalDeviceClassifier.h
 *  Labels for the non-tracker crowd: phones, watches, earbuds, mystery boxes.
 * ===========================================================================
 *
 *  Golden rule: be conservative. A label is only applied when the advert says
 *  so itself (Appearance value, a known Apple/Google/Samsung frame). Anything
 *  else is "Unclassified BLE" and is STILL tracked. We never throw a device
 *  away just because we cannot name it.
 *
 *  And yes, every one of these labels is spoofable. A Raspberry Pi can claim
 *  to be a smartwatch with one line of config.
 * ===========================================================================
 */

#pragma once

#include "TrackerClassifier.h"

namespace antistalker {

class PersonalDeviceClassifier {
  public:
    // Returns a conservative personal-device classification when recognized.
    // Otherwise returns GenericBle: every observed advertiser can still be
    // evaluated for co-presence, but its type must not be guessed.
    static TrackerMatch classify(const uint8_t* payload, size_t length);
};

} // namespace antistalker
