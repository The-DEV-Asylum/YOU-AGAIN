/*
 * ===========================================================================
 *  AntiStalker :: test/test_core/test_main.cpp
 *  The native test suite. Runs on your laptop, no board, no radio, no excuses.
 * ===========================================================================
 *
 *      pio test -e native
 *
 *  What is covered: real advert layouts for every supported tracker family,
 *  malformed/truncated payloads, Eddystone vs Find Hub, the personal-device
 *  labels, every alert-state threshold, address rotation, allow lists, RSSI
 *  filtering, tiered retention, trust/IRK resolution (spec vectors AND a live
 *  capture) and the distance bands.
 *
 *  What is NOT covered: anything needing an actual radio. NimBLE, Wi-Fi and
 *  the web server only get tested by flashing a board and walking around.
 *
 *  Test payloads are hand-built byte arrays. Each one is
 *  [len][type][value...], same as on the air. When in doubt, count bytes.
 * ===========================================================================
 */

#include <unity.h>

#include "DetectionEngine.h"
#include "DistanceEstimate.h"
#include "PersonalDeviceClassifier.h"
#include "TrackerClassifier.h"
#include "TrustResolver.h"

#include <string.h>

using namespace antistalker;

namespace {

void testAppleFindMyAdvertisement() {
    uint8_t payload[31] = {0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10};
    for (size_t i = 7; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 3);
    }

    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::AppleFindMy, match.family);
    TEST_ASSERT_TRUE(match.hasIdentityPayload);
    TEST_ASSERT_FALSE(match.nearOwnerKnown);
    TEST_ASSERT_NOT_EQUAL(0, match.identityHint);

    uint8_t statusChanged[31];
    memcpy(statusChanged, payload, sizeof(payload));
    statusChanged[6] ^= 0x01;
    TEST_ASSERT_EQUAL(match.identityHint,
                      TrackerClassifier::classify(statusChanged, sizeof(statusChanged)).identityHint);
}

void testAppleShortFindMyFrame() {
    const uint8_t airTagFrame[] = {0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x10, 0x00};
    const uint8_t phoneFrame[] = {0x07, 0xFF, 0x4C, 0x00, 0x12, 0x02, 0x00, 0x00};
    TEST_ASSERT_EQUAL(TrackerFamily::AppleFindMy,
                      TrackerClassifier::classify(airTagFrame, sizeof(airTagFrame)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(phoneFrame, sizeof(phoneFrame)).family);
}

void testAppleNonFindMyPayloadsAreIgnored() {
    const uint8_t nearbyInfo[] = {0x05, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0x00};
    const uint8_t iBeacon[] = {0x07, 0xFF, 0x4C, 0x00, 0x02, 0x15, 0x01, 0x02};
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(nearbyInfo, sizeof(nearbyInfo)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(iBeacon, sizeof(iBeacon)).family);
}

void testDultSeparatedAdvertisement() {
    const uint8_t payload[] = {0x07, 0x16, 0xB2, 0xFC, 0x01, 0x00, 0xAA, 0xBB};
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::Dult, match.family);
    TEST_ASSERT_TRUE(match.nearOwnerKnown);
    TEST_ASSERT_FALSE(match.nearOwner);
    TEST_ASSERT_EQUAL(1, match.networkId);
    TEST_ASSERT_TRUE(match.hasIdentityPayload);

    const uint8_t nearOwner[] = {0x07, 0x16, 0xB2, 0xFC, 0x01, 0x01, 0xAA, 0xBB};
    const TrackerMatch ownerMatch = TrackerClassifier::classify(nearOwner, sizeof(nearOwner));
    TEST_ASSERT_TRUE(ownerMatch.nearOwner);
    TEST_ASSERT_EQUAL(match.identityHint, ownerMatch.identityHint);
}

void testDultWithoutProprietaryDataUsesAddressFallback() {
    const uint8_t payload[] = {0x05, 0x16, 0xB2, 0xFC, 0x07, 0x00};
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::Dult, match.family);
    TEST_ASSERT_FALSE(match.hasIdentityPayload);
    TEST_ASSERT_EQUAL(7, match.networkId);
}

void testDultIsPreferredForItsNearOwnerState() {
    const uint8_t payload[] = {
        0x05, 0x16, 0xED, 0xFE, 0x01, 0x02,
        0x05, 0x16, 0xB2, 0xFC, 0x01, 0x01,
    };
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::Dult, match.family);
    TEST_ASSERT_TRUE(match.nearOwner);
}

void testGoogleFindHubAdvertisement() {
    uint8_t payload[25] = {0x18, 0x16, 0xAA, 0xFE, 0x40};
    for (size_t i = 5; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 5);
    }
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::GoogleFindHub, match.family);
    TEST_ASSERT_TRUE(match.hasIdentityPayload);

    uint8_t withHashedFlags[26] = {0x19, 0x16, 0xAA, 0xFE, 0x41};
    memcpy(withHashedFlags + 5, payload + 5, 20);
    withHashedFlags[25] = 0xA5;
    const TrackerMatch flagged = TrackerClassifier::classify(withHashedFlags, sizeof(withHashedFlags));
    TEST_ASSERT_EQUAL(TrackerFamily::GoogleFindHub, flagged.family);
    TEST_ASSERT_EQUAL(match.identityHint, flagged.identityHint);

    uint8_t extended[37] = {0x24, 0x16, 0xAA, 0xFE, 0x41};
    for (size_t i = 5; i < sizeof(extended); ++i) {
        extended[i] = static_cast<uint8_t>(i * 7);
    }
    TEST_ASSERT_EQUAL(TrackerFamily::GoogleFindHub,
                      TrackerClassifier::classify(extended, sizeof(extended)).family);
}

void testEddystoneAndInvalidFindHubFramesAreIgnored() {
    const uint8_t eddystone[] = {0x05, 0x16, 0xAA, 0xFE, 0x00, 0x01};
    const uint8_t wrongLength[] = {0x05, 0x16, 0xAA, 0xFE, 0x40, 0x01};
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(eddystone, sizeof(eddystone)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(wrongLength, sizeof(wrongLength)).family);
}

void testSamsungSmartTagAdvertisement() {
    uint8_t payload[24] = {0x17, 0x16, 0x5A, 0xFD};
    for (size_t i = 4; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i);
    }
    payload[4] = 0x11; // Registered offline-finding frame, offline state 1.
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::SamsungSmartTag, match.family);
    TEST_ASSERT_TRUE(match.hasIdentityPayload);
    TEST_ASSERT_TRUE(match.nearOwnerKnown);
    TEST_ASSERT_FALSE(match.nearOwner);

    uint8_t connected[24];
    memcpy(connected, payload, sizeof(payload));
    connected[4] = 0x14;
    TEST_ASSERT_TRUE(TrackerClassifier::classify(connected, sizeof(connected)).nearOwner);

    uint8_t agingChanged[24];
    memcpy(agingChanged, payload, sizeof(payload));
    agingChanged[5] ^= 0xFF;
    TEST_ASSERT_EQUAL(match.identityHint,
                      TrackerClassifier::classify(agingChanged, sizeof(agingChanged)).identityHint);
}

void testTileAdvertisement() {
    const uint8_t payload[] = {0x07, 0x16, 0xED, 0xFE, 0x01, 0x02, 0x03, 0x04};
    const TrackerMatch match = TrackerClassifier::classify(payload, sizeof(payload));
    TEST_ASSERT_EQUAL(TrackerFamily::Tile, match.family);
    TEST_ASSERT_TRUE(match.hasIdentityPayload);
}

void testNativeChipoloAndPebblebeeServiceUuids() {
    const uint8_t chipolo[] = {0x03, 0x03, 0x33, 0xFE};
    const uint8_t pebblebee[] = {0x03, 0x02, 0x25, 0xFA};
    TEST_ASSERT_EQUAL(TrackerFamily::Chipolo,
                      TrackerClassifier::classify(chipolo, sizeof(chipolo)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::Pebblebee,
                      TrackerClassifier::classify(pebblebee, sizeof(pebblebee)).family);
}

void testUnknownAndMalformedPayloadsAreSafe() {
    const uint8_t unknown[] = {0x04, 0x16, 0xAA, 0xFE, 0x00};
    const uint8_t truncated[] = {0x20, 0xFF, 0x4C, 0x00};
    const uint8_t invalid[] = {0x00, 0xFF, 0x4C};
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(unknown, sizeof(unknown)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(truncated, sizeof(truncated)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None,
                      TrackerClassifier::classify(invalid, sizeof(invalid)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None, TrackerClassifier::classify(nullptr, 20).family);
}

void testPersonalAppearanceClassification() {
    const uint8_t phone[] = {0x03, 0x19, 0x40, 0x00};
    const uint8_t smartwatch[] = {0x03, 0x19, 0xC2, 0x00};
    const uint8_t earbud[] = {0x03, 0x19, 0x41, 0x09};
    const uint8_t speaker[] = {0x03, 0x19, 0x40, 0x08};
    TEST_ASSERT_EQUAL(TrackerFamily::Phone, PersonalDeviceClassifier::classify(phone, sizeof(phone)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::Watch,
                      PersonalDeviceClassifier::classify(smartwatch, sizeof(smartwatch)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::AudioDevice,
                      PersonalDeviceClassifier::classify(earbud, sizeof(earbud)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::AudioDevice,
                      PersonalDeviceClassifier::classify(speaker, sizeof(speaker)).family);
}

void testAppleContinuityAndProximityClassification() {
    const uint8_t nearby[] = {0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x05, 1, 2, 3, 4, 5};
    TEST_ASSERT_EQUAL(TrackerFamily::AppleContinuityDevice,
                      PersonalDeviceClassifier::classify(nearby, sizeof(nearby)).family);

    uint8_t audio[31] = {0x1E, 0xFF, 0x4C, 0x00, 0x07, 0x19, 0x01};
    for (size_t i = 7; i < sizeof(audio); ++i) {
        audio[i] = static_cast<uint8_t>(i);
    }
    TEST_ASSERT_EQUAL(TrackerFamily::AudioDevice,
                      PersonalDeviceClassifier::classify(audio, sizeof(audio)).family);

    const uint8_t shortProximity[] = {0x06, 0xFF, 0x4C, 0x00, 0x07, 0x01, 0x00};
    TEST_ASSERT_EQUAL(TrackerFamily::AppleProximityAccessory,
                      PersonalDeviceClassifier::classify(shortProximity, sizeof(shortProximity)).family);
}

void testFastPairAndSamsungOfflineFindingClassification() {
    const uint8_t fastPair[] = {0x06, 0x16, 0x2C, 0xFE, 0xAA, 0xBB, 0xCC};
    TEST_ASSERT_EQUAL(TrackerFamily::FastPairAccessory,
                      PersonalDeviceClassifier::classify(fastPair, sizeof(fastPair)).family);

    uint8_t samsung[18] = {0x11, 0x16, 0x69, 0xFD};
    for (size_t i = 4; i < sizeof(samsung); ++i) {
        samsung[i] = static_cast<uint8_t>(i);
    }
    TEST_ASSERT_EQUAL(TrackerFamily::SamsungMobileDevice,
                      PersonalDeviceClassifier::classify(samsung, sizeof(samsung)).family);

    const uint8_t uuidOnly[] = {0x03, 0x03, 0x69, 0xFD};
    TEST_ASSERT_EQUAL(TrackerFamily::GenericBle,
                      PersonalDeviceClassifier::classify(uuidOnly, sizeof(uuidOnly)).family);
}

void testGenericBleFallback() {
    const uint8_t unknown[] = {0x02, 0x01, 0x06};
    TEST_ASSERT_EQUAL(TrackerFamily::GenericBle,
                      PersonalDeviceClassifier::classify(unknown, sizeof(unknown)).family);
    TEST_ASSERT_EQUAL(TrackerFamily::GenericBle, PersonalDeviceClassifier::classify(unknown, 0).family);
    TEST_ASSERT_EQUAL(TrackerFamily::None, PersonalDeviceClassifier::classify(nullptr, 0).family);
}

Observation observation(uint32_t nowMs, const char* address = "aa:bb:cc:dd:ee:ff",
                        uint32_t identity = 0x12345678) {
    Observation result;
    result.nowMs = nowMs;
    result.address = address;
    result.rssi = -60;
    result.match.family = TrackerFamily::AppleFindMy;
    result.match.identityHint = identity;
    result.match.hasIdentityPayload = true;
    return result;
}

void testNewTrackerStartsNearby() {
    DetectionEngine engine;
    const EngineUpdate update = engine.record(observation(0));
    TEST_ASSERT_TRUE(update.accepted);
    TEST_ASSERT_TRUE(update.created);
    TEST_ASSERT_FALSE(update.alert);
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
}

void testPersistenceNeedsTimeAndCoverage() {
    DetectionEngine engine;
    engine.record(observation(0));
    engine.record(observation(1000));
    engine.record(observation(60000));
    EngineUpdate update = engine.record(observation(120000));
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_FALSE(update.alert);

    update = engine.record(observation(600000));
    TEST_ASSERT_EQUAL(AlertLevel::Persistent, update.level);
    TEST_ASSERT_TRUE(update.alert);
    TEST_ASSERT_EQUAL(1, engine.activeAlertCount());
}

void testBurstOfPacketsIsNotPersistent() {
    DetectionEngine engine;
    EngineUpdate update;
    for (uint32_t i = 0; i < 20; ++i) {
        update = engine.record(observation(i * 100));
    }
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_EQUAL(1, engine.find(update.id)->distinctMinutes);
}

void testPersonalDevicesDoNotEscalateFromTimeAlone() {
    DetectionConfig config;
    config.enablePersistentState = false;
    DetectionEngine engine(config);
    Observation sample = observation(0);
    sample.match.family = TrackerFamily::Phone;
    sample.match.hasIdentityPayload = false;
    sample.addressType = 1;
    engine.record(sample);
    sample.nowMs = 60000;
    engine.record(sample);
    sample.nowMs = 120000;
    engine.record(sample);
    sample.nowMs = 600000;
    const EngineUpdate update = engine.record(sample);
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_FALSE(update.alert);
}

void testAddressTypeSeparatesMacFallbackIdentities() {
    DetectionEngine engine;
    Observation sample = observation(0);
    sample.match.family = TrackerFamily::GenericBle;
    sample.match.hasIdentityPayload = false;
    sample.addressType = 0;
    engine.record(sample);
    sample.nowMs = 1000;
    sample.addressType = 1;
    engine.record(sample);
    TEST_ASSERT_EQUAL(2, engine.candidateCount());
}

void testFollowingNeedsTwoConfirmedLocations() {
    DetectionEngine engine;
    engine.record(observation(0));
    engine.record(observation(1000));
    engine.markLocationChanged();
    EngineUpdate update = engine.record(observation(60000));
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    update = engine.record(observation(61000));
    TEST_ASSERT_EQUAL(AlertLevel::Following, update.level);
    TEST_ASSERT_TRUE(update.alert);
    TEST_ASSERT_EQUAL(2, engine.find(update.id)->locations);
}

void testOneSightingPerLocationDoesNotTriggerFollowing() {
    DetectionEngine engine;
    engine.record(observation(0));
    engine.markLocationChanged();
    const EngineUpdate update = engine.record(observation(120000));
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_EQUAL(2, engine.find(update.id)->locations);
}

void testPayloadIdentitySurvivesAddressRotation() {
    DetectionEngine engine;
    const EngineUpdate first = engine.record(observation(0, "aa:aa:aa:aa:aa:aa"));
    const EngineUpdate second = engine.record(observation(1000, "bb:bb:bb:bb:bb:bb"));
    TEST_ASSERT_EQUAL(first.id, second.id);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
    TEST_ASSERT_EQUAL_STRING("BB:BB:BB:BB:BB:BB", engine.find(first.id)->address);
}

void testDifferentPayloadsWithSameAddressStaySeparate() {
    DetectionEngine engine;
    engine.record(observation(0, "aa:aa:aa:aa:aa:aa", 1));
    engine.record(observation(1000, "aa:aa:aa:aa:aa:aa", 2));
    TEST_ASSERT_EQUAL(2, engine.candidateCount());
}

void testNearOwnerSuppressesEscalation() {
    DetectionEngine engine;
    Observation separated = observation(0);
    separated.match.nearOwnerKnown = true;
    separated.match.nearOwner = false;
    engine.record(separated);
    separated.nowMs = 1000;
    engine.record(separated);

    Observation owned = separated;
    owned.nowMs = 60000;
    owned.match.nearOwner = true;
    engine.record(owned);
    owned.nowMs = 61000;
    const EngineUpdate update = engine.record(owned);
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_FALSE(update.alert);
    TEST_ASSERT_EQUAL(0, engine.activeAlertCount());
}

void testUnknownOwnerStateClearsPreviousInterpretation() {
    DetectionEngine engine;
    Observation sample = observation(0);
    sample.match.nearOwnerKnown = true;
    sample.match.nearOwner = true;
    const EngineUpdate created = engine.record(sample);

    sample.nowMs = 1000;
    sample.match.nearOwnerKnown = false;
    sample.match.nearOwner = false;
    engine.record(sample);
    TEST_ASSERT_FALSE(engine.find(created.id)->nearOwnerKnown);
    TEST_ASSERT_FALSE(engine.find(created.id)->nearOwner);
}

void testAllowListSuppressesAlerts() {
    DetectionEngine engine;
    const EngineUpdate created = engine.record(observation(0));
    TEST_ASSERT_TRUE(engine.allow(created.id));
    engine.record(observation(1000));
    engine.markLocationChanged();
    engine.record(observation(60000));
    const EngineUpdate update = engine.record(observation(61000));
    TEST_ASSERT_EQUAL(AlertLevel::Following, update.level);
    TEST_ASSERT_FALSE(update.alert);
    TEST_ASSERT_EQUAL(0, engine.activeAlertCount());
    TEST_ASSERT_TRUE(engine.allow(created.id, false));
}

void testRssiFilteringAndStrongestSignal() {
    DetectionEngine engine;
    const EngineUpdate created = engine.record(observation(0));
    Observation sample = observation(1000);
    sample.rssi = -40;
    engine.record(sample);
    const Candidate* candidate = engine.find(created.id);
    TEST_ASSERT_EQUAL(-55, candidate->filteredRssi);
    TEST_ASSERT_EQUAL(-40, candidate->strongestRssi);
}

void testClearAndPruneReleaseCandidates() {
    DetectionConfig config;
    config.candidateExpiryMs = 1000;
    DetectionEngine engine(config);
    engine.record(observation(0));
    engine.prune(1001);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
    engine.record(observation(2000));
    engine.clear();
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
    TEST_ASSERT_EQUAL(0, engine.locationEpoch());
}

void testUnremarkableNearbyCandidatesExpireQuickly() {
    // 64 slots pinned full of one-off rotating phones is a useless table, so
    // single-location NEARBY noise must NOT get the full 6-hour retention.
    DetectionEngine engine;
    const uint32_t stale = DetectionConfig().staleNearbyExpiryMs;
    engine.record(observation(0, "cc:cc:cc:cc:cc:01"));
    engine.prune(stale - 1);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
    engine.prune(stale + 1);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
}

void testRepeatedSingleLocationHistoryIsRetainedLonger() {
    DetectionEngine engine;
    const DetectionConfig config;
    const uint32_t start = 1000;
    for (int i = 0; i < config.minimumSightings; ++i) {
        engine.record(observation(start + i, "cc:cc:cc:cc:cc:02", 0x11111102));
    }
    engine.prune(start + config.staleNearbyExpiryMs);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
    engine.prune(start + config.staleActiveExpiryMs + 1000);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
}

void testMultiLocationAndAllowedCandidatesKeepFullRetention() {
    DetectionEngine engine;
    const DetectionConfig config;

    const EngineUpdate traveller =
        engine.record(observation(0, "cc:cc:cc:cc:cc:03", 0x11111103));
    engine.markLocationChanged();
    engine.record(observation(60000, "cc:cc:cc:cc:cc:03", 0x11111103));
    TEST_ASSERT_EQUAL(2, engine.find(traveller.id)->locations);

    const EngineUpdate allowed = engine.record(observation(0, "cc:cc:cc:cc:cc:04", 0x11111104));
    engine.allow(allowed.id);

    engine.prune(config.staleActiveExpiryMs + 61000);
    TEST_ASSERT_EQUAL(2, engine.candidateCount());
    engine.prune(config.candidateExpiryMs + 61000);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
}

void testElevatedCandidatesKeepFullRetention() {
    DetectionEngine engine;
    const DetectionConfig config;
    const uint32_t start = 1000;
    // 4 sightings across 3 distinct minutes over 10 minutes => PERSISTENT.
    engine.record(observation(start, "cc:cc:cc:cc:cc:05", 0x11111105));
    engine.record(observation(start + 60000, "cc:cc:cc:cc:cc:05", 0x11111105));
    engine.record(observation(start + 120000, "cc:cc:cc:cc:cc:05", 0x11111105));
    const EngineUpdate update = engine.record(
        observation(start + config.persistentAfterMs, "cc:cc:cc:cc:cc:05", 0x11111105));
    TEST_ASSERT_EQUAL(AlertLevel::Persistent, update.level);

    engine.prune(start + config.staleActiveExpiryMs + config.persistentAfterMs);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
    engine.prune(start + config.candidateExpiryMs + config.persistentAfterMs + 1000);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
}

void testTrustedDeviceIsAutoAllowedAndCappedAtNearby() {
    DetectionEngine engine;
    Observation sample = observation(0, "54:EB:E9:77:8A:EA", 0x1111110A);
    sample.trusted = true;
    const EngineUpdate created = engine.record(sample);
    const Candidate* candidate = engine.find(created.id);
    TEST_ASSERT_TRUE(candidate->trusted);
    TEST_ASSERT_TRUE(candidate->allowed);

    // Feed it exactly what escalates a stranger (minutes of history, two
    // confirmed locations). A bonded device must shrug and stay NEARBY.
    sample.nowMs = 1000;
    engine.record(sample);
    engine.markLocationChanged();
    sample.nowMs = 60000;
    engine.record(sample);
    sample.nowMs = 61000;
    const EngineUpdate update = engine.record(sample);
    TEST_ASSERT_EQUAL(AlertLevel::Nearby, update.level);
    TEST_ASSERT_FALSE(update.alert);
    TEST_ASSERT_EQUAL(0, engine.activeAlertCount());
}

void testTrustedUnallowIsNoOp() {
    DetectionEngine engine;
    Observation sample = observation(0, "54:EB:E9:77:8A:EA", 0x1111110B);
    sample.trusted = true;
    const EngineUpdate created = engine.record(sample);
    TEST_ASSERT_TRUE(engine.allow(created.id, false));
    TEST_ASSERT_TRUE(engine.find(created.id)->allowed); // Bond beats the list.
    TEST_ASSERT_TRUE(engine.allow(created.id, true));
}

void testTrustedIdentityKeepsOneIdAcrossClassifications() {
    DetectionEngine engine;
    Observation phoneFrame = observation(0, "54:EB:E9:77:8A:EA", 0x1111110C);
    phoneFrame.trusted = true;
    const EngineUpdate first = engine.record(phoneFrame);

    // Same bonded identity seen later via a frame with a completely different
    // payload-derived identity hint: one device, one candidate.
    Observation continuity = observation(1000, "54:EB:E9:77:8A:EA", 0x22222222);
    continuity.trusted = true;
    const EngineUpdate second = engine.record(continuity);
    TEST_ASSERT_EQUAL(first.id, second.id);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());

    // Without trust, the same two hints are two separate observations.
    DetectionEngine plain;
    plain.record(observation(0, "54:EB:E9:77:8A:EA", 0x1111110C));
    plain.record(observation(1000, "54:EB:E9:77:8A:EA", 0x22222222));
    TEST_ASSERT_EQUAL(2, plain.candidateCount());
}

void testTrustedCandidatesKeepFullRetention() {
    DetectionEngine engine;
    const DetectionConfig config;
    Observation sample = observation(0, "cc:cc:cc:cc:cc:06", 0x1111110D);
    sample.trusted = true;
    engine.record(sample);
    engine.prune(config.staleNearbyExpiryMs + 1000);
    TEST_ASSERT_EQUAL(1, engine.candidateCount());
    engine.prune(config.candidateExpiryMs + 1000);
    TEST_ASSERT_EQUAL(0, engine.candidateCount());
}

void testResolvablePrivateAddressShape() {
    uint8_t resolvable[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t staticRandom[6] = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t nonResolvable[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t topHalf[6] = {0x7F, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t bottomHalf[6] = {0x80, 0x00, 0x00, 0x00, 0x00, 0x00};
    TEST_ASSERT_TRUE(TrustResolver::isResolvablePrivateAddress(resolvable));
    TEST_ASSERT_TRUE(TrustResolver::isResolvablePrivateAddress(topHalf));
    TEST_ASSERT_FALSE(TrustResolver::isResolvablePrivateAddress(staticRandom));
    TEST_ASSERT_FALSE(TrustResolver::isResolvablePrivateAddress(nonResolvable));
    TEST_ASSERT_FALSE(TrustResolver::isResolvablePrivateAddress(bottomHalf));
}

void testIrkAddressHashMatchesReferenceVectors() {
    // Independent AES-128 reference values (openssl enc -aes-128-ecb -nopad)
    // for the ah() construction of Bluetooth Core Spec Vol 3 Part H 2.2.2.
    // The IRK is given in air order (as stored); addressHash reverses it into
    // the AES key, so reference keys below are byte-reversed.
    const uint8_t irk[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t address[6] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t hash[3];
    TrustResolver::addressHash(irk, address, hash);
    TEST_ASSERT_EQUAL_UINT8(0x87, hash[0]);
    TEST_ASSERT_EQUAL_UINT8(0x1F, hash[1]);
    TEST_ASSERT_EQUAL_UINT8(0x64, hash[2]);

    address[0] = 0x44;
    address[1] = 0x53;
    address[2] = 0x18;
    TrustResolver::addressHash(irk, address, hash);
    TEST_ASSERT_EQUAL_UINT8(0x9C, hash[0]);
    TEST_ASSERT_EQUAL_UINT8(0x51, hash[1]);
    TEST_ASSERT_EQUAL_UINT8(0x1C, hash[2]);

    const uint8_t fipsIrk[16] = {0x2B, 0x7E, 0x15, 0x16, 0x28, 0xAE, 0xD2, 0xA6,
                                 0xAB, 0xF7, 0x15, 0x88, 0x09, 0xCF, 0x4F, 0x3C};
    TrustResolver::addressHash(fipsIrk, address, hash);
    TEST_ASSERT_EQUAL_UINT8(0x5C, hash[0]);
    TEST_ASSERT_EQUAL_UINT8(0xB8, hash[1]);
    TEST_ASSERT_EQUAL_UINT8(0x88, hash[2]);
}

void testResolveLiveCapturedPhoneAddress() {
    // Ground truth captured from a bonded Samsung phone (2026-11-22): the IRK
    // exactly as pairing stored it, and the rotating address the phone used
    // for a real connection. THIS is the vector that caught the air-order vs
    // AES-order key mismatch. It looks like a magic constant. It is a
    // magic constant. Do not delete it.
    TrustResolver resolver;
    const uint8_t irk[16] = {0x4F, 0x83, 0xCD, 0x46, 0xF3, 0x3D, 0x15, 0xF0,
                             0x10, 0xB8, 0xED, 0x68, 0x2A, 0x7C, 0xCF, 0xB1};
    const uint8_t identity[6] = {0xBC, 0xA5, 0x8B, 0x1F, 0x78, 0xFC};
    TEST_ASSERT_TRUE(resolver.addIdentity("phone", irk, identity, 0));

    uint8_t rpa[6] = {0x66, 0xE7, 0xA0, 0x33, 0x5C, 0x28};
    TrustIdentity* found = resolver.resolve(rpa, nullptr);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("phone", found->name);
    // One flipped hash bit must break the match.
    rpa[5] ^= 0x01;
    TEST_ASSERT_NULL(resolver.resolve(rpa, nullptr));
}

void testResolveRotatingAddressWithBondedIrk() {
    TrustResolver resolver;
    const uint8_t irk[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8_t identity[6] = {0x54, 0xEB, 0xE9, 0x77, 0x8A, 0xEA};
    TEST_ASSERT_TRUE(resolver.addIdentity("My phone", irk, identity,
                                          TrustResolver::kRandomAddressType));

    // Rotation 1: prand 44:53:18 with the matching hash 9C:51:1C.
    uint8_t rpa1[6] = {0x44, 0x53, 0x18, 0x9C, 0x51, 0x1C};
    TrustIdentity* found = resolver.resolve(rpa1, nullptr);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("My phone", found->name);

    // Rotation 2 of the same identity resolves to the same table entry.
    uint8_t rpa2[6] = {0x40, 0x00, 0x00, 0x87, 0x1F, 0x64};
    TrustIdentity* second = resolver.resolve(rpa2, nullptr);
    TEST_ASSERT_EQUAL_PTR(found, second);
    TEST_ASSERT_EQUAL(2, second->resolvedCount);

    // A corrupted hash does not resolve.
    uint8_t broken[6] = {0x44, 0x53, 0x18, 0x9C, 0x51, 0x1D};
    TEST_ASSERT_NULL(resolver.resolve(broken, nullptr));
    // The static-random prefix is not a resolvable private address at all.
    uint8_t staticRandom[6] = {0xC4, 0x53, 0x18, 0x9C, 0x51, 0x1C};
    TEST_ASSERT_NULL(resolver.resolve(staticRandom, nullptr));
}

void testResolveCapturesAdvertisedNameOnce() {
    TrustResolver resolver;
    const uint8_t irk[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8_t identity[6] = {0x54, 0xEB, 0xE9, 0x77, 0x8A, 0xEB};
    TEST_ASSERT_TRUE(resolver.addIdentity(nullptr, irk, identity,
                                          TrustResolver::kRandomAddressType));

    uint8_t rpa[6] = {0x44, 0x53, 0x18, 0x9C, 0x51, 0x1C};
    TrustIdentity* found = resolver.resolve(rpa, "YouWish");
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_STRING("YouWish", found->name);

    // A later advertised name must not silently overwrite the stored one.
    TrustIdentity* again = resolver.resolve(rpa, "SomethingElse");
    TEST_ASSERT_EQUAL_STRING("YouWish", again->name);
}

void testTrustTableManagement() {
    TrustResolver resolver;
    const uint8_t irk[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t address1[6] = {0x54, 0xEB, 0xE9, 0x77, 0x8A, 0xE1};
    uint8_t address2[6] = {0x54, 0xEB, 0xE9, 0x77, 0x8A, 0xE2};
    uint8_t address3[6] = {0x54, 0xEB, 0xE9, 0x77, 0x8A, 0xE3};

    TEST_ASSERT_TRUE(resolver.addIdentity("phone", irk, address1, 1));
    TEST_ASSERT_TRUE(resolver.addIdentity("watch", irk, address2, 1));
    TEST_ASSERT_TRUE(resolver.addIdentity("tablet", irk, address3, 0));
    TEST_ASSERT_EQUAL(3, resolver.count());
    TEST_ASSERT_NOT_NULL(resolver.findIdentity(address2, 1));
    TEST_ASSERT_NULL(resolver.findIdentity(address2, 0)); // Address type matters.

    TEST_ASSERT_TRUE(resolver.setName(address1, 1, "YouWish"));
    TEST_ASSERT_EQUAL_STRING("YouWish", resolver.findIdentity(address1, 1)->name);

    // Removing compacts the table so persistence slots stay dense.
    TEST_ASSERT_TRUE(resolver.removeIdentity(address1, 1));
    TEST_ASSERT_EQUAL(2, resolver.count());
    TEST_ASSERT_NULL(resolver.findIdentity(address1, 1));
    TEST_ASSERT_EQUAL_STRING("watch", resolver.identityAt(0)->name);
    TEST_ASSERT_EQUAL_STRING("tablet", resolver.identityAt(1)->name);
    TEST_ASSERT_FALSE(resolver.identityAt(2)->used);
    TEST_ASSERT_FALSE(resolver.removeIdentity(address1, 1));

    TrustResolver full;
    uint8_t address[6] = {0x11, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (uint8_t i = 0; i < 4; ++i) {
        address[5] = i;
        TEST_ASSERT_TRUE(full.addIdentity("x", irk, address, 1));
    }
    address[5] = 9;
    TEST_ASSERT_FALSE(full.addIdentity("x", irk, address, 1));
    TEST_ASSERT_EQUAL(4, full.count());

    // Re-pairing the same identity refreshes its key in place, keeping the name.
    const uint8_t freshIrk[16] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    address[5] = 0;
    TEST_ASSERT_TRUE(full.addIdentity("Trusted 9", freshIrk, address, 1));
    TEST_ASSERT_EQUAL(4, full.count());
    TEST_ASSERT_EQUAL_MEMORY(freshIrk, full.findIdentity(address, 1)->irk, 16);
    // The auto-generated re-pairing name must not overwrite the owner's name.
    TEST_ASSERT_EQUAL_STRING("x", full.findIdentity(address, 1)->name);
}

void testAddressParsingAndFormatting() {
    uint8_t parsed[6];
    TEST_ASSERT_TRUE(TrustResolver::parseAddress("54:eb:E9-77 8a:ef", parsed));
    TEST_ASSERT_EQUAL_UINT8(0x54, parsed[0]);
    TEST_ASSERT_EQUAL_UINT8(0xEF, parsed[5]);
    TEST_ASSERT_TRUE(TrustResolver::parseAddress("54EBE9778AEA", parsed));
    TEST_ASSERT_EQUAL_UINT8(0xEA, parsed[5]);

    char formatted[18];
    TrustResolver::formatAddress(parsed, formatted, sizeof(formatted));
    TEST_ASSERT_EQUAL_STRING("54:EB:E9:77:8A:EA", formatted);

    TEST_ASSERT_FALSE(TrustResolver::parseAddress("54:EB:E9:77:8A", parsed));
    TEST_ASSERT_FALSE(TrustResolver::parseAddress("54:EB:E9:77:8A:EG", parsed));
    TEST_ASSERT_FALSE(TrustResolver::parseAddress("54:EB:E9:77:8A:EA:42", parsed));
    TEST_ASSERT_FALSE(TrustResolver::parseAddress("", parsed));
    TEST_ASSERT_FALSE(TrustResolver::parseAddress(nullptr, parsed));
}

void testDistanceEstimatesAreMonotonicAndBanded() {
    TEST_ASSERT_EQUAL(1, estimateDistanceM(-59)); // Reference: 1 m at -59 dBm.
    TEST_ASSERT_EQUAL(3, estimateDistanceM(-69));
    TEST_ASSERT_EQUAL(100, estimateDistanceM(-127)); // Clamped far end.
    TEST_ASSERT_EQUAL(1, estimateDistanceM(-40));     // Clamped near end.
    TEST_ASSERT_TRUE(estimateDistanceM(-69) > estimateDistanceM(-59));
    TEST_ASSERT_TRUE(estimateDistanceM(-90) > estimateDistanceM(-70));

    TEST_ASSERT_EQUAL_STRING("under 1 m", distanceBand(-50));
    TEST_ASSERT_EQUAL_STRING("about 1-4 m", distanceBand(-70));
    TEST_ASSERT_EQUAL_STRING("about 4-10 m", distanceBand(-80));
    TEST_ASSERT_EQUAL_STRING("about 10-30 m", distanceBand(-90));
    TEST_ASSERT_EQUAL_STRING("over 30 m", distanceBand(-100));
}

void runTests() {
    UNITY_BEGIN();
    RUN_TEST(testAppleFindMyAdvertisement);
    RUN_TEST(testAppleShortFindMyFrame);
    RUN_TEST(testAppleNonFindMyPayloadsAreIgnored);
    RUN_TEST(testDultSeparatedAdvertisement);
    RUN_TEST(testDultWithoutProprietaryDataUsesAddressFallback);
    RUN_TEST(testDultIsPreferredForItsNearOwnerState);
    RUN_TEST(testGoogleFindHubAdvertisement);
    RUN_TEST(testEddystoneAndInvalidFindHubFramesAreIgnored);
    RUN_TEST(testSamsungSmartTagAdvertisement);
    RUN_TEST(testTileAdvertisement);
    RUN_TEST(testNativeChipoloAndPebblebeeServiceUuids);
    RUN_TEST(testUnknownAndMalformedPayloadsAreSafe);
    RUN_TEST(testPersonalAppearanceClassification);
    RUN_TEST(testAppleContinuityAndProximityClassification);
    RUN_TEST(testFastPairAndSamsungOfflineFindingClassification);
    RUN_TEST(testGenericBleFallback);
    RUN_TEST(testNewTrackerStartsNearby);
    RUN_TEST(testPersistenceNeedsTimeAndCoverage);
    RUN_TEST(testBurstOfPacketsIsNotPersistent);
    RUN_TEST(testPersonalDevicesDoNotEscalateFromTimeAlone);
    RUN_TEST(testAddressTypeSeparatesMacFallbackIdentities);
    RUN_TEST(testFollowingNeedsTwoConfirmedLocations);
    RUN_TEST(testOneSightingPerLocationDoesNotTriggerFollowing);
    RUN_TEST(testPayloadIdentitySurvivesAddressRotation);
    RUN_TEST(testDifferentPayloadsWithSameAddressStaySeparate);
    RUN_TEST(testNearOwnerSuppressesEscalation);
    RUN_TEST(testUnknownOwnerStateClearsPreviousInterpretation);
    RUN_TEST(testAllowListSuppressesAlerts);
    RUN_TEST(testRssiFilteringAndStrongestSignal);
    RUN_TEST(testClearAndPruneReleaseCandidates);
    RUN_TEST(testUnremarkableNearbyCandidatesExpireQuickly);
    RUN_TEST(testRepeatedSingleLocationHistoryIsRetainedLonger);
    RUN_TEST(testMultiLocationAndAllowedCandidatesKeepFullRetention);
    RUN_TEST(testElevatedCandidatesKeepFullRetention);
    RUN_TEST(testTrustedDeviceIsAutoAllowedAndCappedAtNearby);
    RUN_TEST(testTrustedUnallowIsNoOp);
    RUN_TEST(testTrustedIdentityKeepsOneIdAcrossClassifications);
    RUN_TEST(testTrustedCandidatesKeepFullRetention);
    RUN_TEST(testResolvablePrivateAddressShape);
    RUN_TEST(testIrkAddressHashMatchesReferenceVectors);
    RUN_TEST(testResolveLiveCapturedPhoneAddress);
    RUN_TEST(testResolveRotatingAddressWithBondedIrk);
    RUN_TEST(testResolveCapturesAdvertisedNameOnce);
    RUN_TEST(testTrustTableManagement);
    RUN_TEST(testAddressParsingAndFormatting);
    RUN_TEST(testDistanceEstimatesAreMonotonicAndBanded);
    UNITY_END();
}

} // namespace

int main(int, char**) {
    runTests();
    return 0;
}
