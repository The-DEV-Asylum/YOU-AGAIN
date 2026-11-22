/*
 * ===========================================================================
 *  AntiStalker :: DetectionEngine.h
 *  The brain. Turns a stream of sightings into NEARBY / PERSISTENT / FOLLOWING.
 * ===========================================================================
 *
 *  State machine, per candidate:
 *
 *      NEARBY  -----(10+ min, 4+ sightings, 3+ distinct minutes)----> PERSISTENT
 *        |                                                   (trackers only)
 *        |
 *        +------(seen 2+ times at EACH of 2+ confirmed locations,
 *                4+ sightings, 60+ s total)------------------------> FOLLOWING
 *                                       (shown as CO-PRESENT for non-trackers)
 *
 *      Trusted (bonded) device or "near owner" frame -> always NEARBY.
 *
 *  A "location" is a location EPOCH: a counter bumped when the user says
 *  "I moved" or the phone GPS says so. The engine has no idea where you are,
 *  it only knows "this is the 3rd place since boot". Privacy by ignorance.
 *
 *  Fixed table of 64 slots, no heap. When full, the stalest candidate gets
 *  evicted. On an MCU, "unbounded" is just a slower way of saying "crash".
 * ===========================================================================
 */

#pragma once

#include "TrackerClassifier.h"

#include <stddef.h>
#include <stdint.h>

namespace antistalker {

constexpr size_t kMaxCandidates = 64;

struct DetectionConfig {
    uint32_t persistentAfterMs = 10UL * 60UL * 1000UL;
    uint32_t followingMinDurationMs = 60UL * 1000UL;
    uint32_t candidateExpiryMs = 6UL * 60UL * 60UL * 1000UL;
    // Boring NEARBY candidates expire much sooner than the full 6 h. Lesson
    // from the field: phones rotate their MAC every ~15 min, so without this
    // both tables sat at 64/64 forever and the counts meant nothing.
    uint32_t staleNearbyExpiryMs = 15UL * 60UL * 1000UL;   // Few sightings only.
    uint32_t staleActiveExpiryMs = 60UL * 60UL * 1000UL;  // Single-location history.
    uint16_t minimumSightings = 4;
    uint8_t minimumDistinctMinutes = 3;
    bool enablePersistentState = true;
};

enum class AlertLevel : uint8_t {
    Nearby = 0,
    Persistent,
    Following,
};

struct Observation {
    const char* address = nullptr;
    uint8_t addressType = 0xFF;
    int rssi = -127;
    uint32_t nowMs = 0;
    // True when an IRK resolved the rotating address: it is one of OUR
    // bonded devices. Trusted devices never raise risk states. Ever.
    bool trusted = false;
    TrackerMatch match;
};

struct Candidate {
    bool used = false;
    bool allowed = false;
    bool trusted = false;
    uint32_t id = 0;
    TrackerFamily family = TrackerFamily::None;
    char address[18] = {0};
    uint8_t addressType = 0xFF;
    uint32_t firstSeenMs = 0;
    uint32_t lastSeenMs = 0;
    uint32_t sightings = 0;
    uint8_t distinctMinutes = 0;
    uint8_t locations = 0;
    uint8_t multiSightingLocations = 0;
    int filteredRssi = -127;
    int strongestRssi = -127;
    bool nearOwnerKnown = false;
    bool nearOwner = false;
    uint8_t networkId = 0;
    AlertLevel level = AlertLevel::Nearby;

    // Internal bookkeeping (per candidate, so two engines never share state).
    // 0xFFFFFFFF = "never", so the first sighting always counts as new.
    uint32_t lastEpoch = 0xFFFFFFFFUL;
    uint32_t lastMinute = 0xFFFFFFFFUL;
    uint8_t sightingsThisLocation = 0;
};

struct EngineUpdate {
    bool accepted = false;
    bool created = false;
    bool levelChanged = false;
    bool alert = false;
    uint32_t id = 0;
    AlertLevel previousLevel = AlertLevel::Nearby;
    AlertLevel level = AlertLevel::Nearby;
};

class DetectionEngine {
  public:
    explicit DetectionEngine(DetectionConfig config = DetectionConfig());

    EngineUpdate record(const Observation& observation);
    void markLocationChanged();
    void prune(uint32_t nowMs);
    void clear();

    bool allow(uint32_t id, bool allowed = true);
    const Candidate* find(uint32_t id) const;
    const Candidate* candidateAt(size_t index) const;
    size_t candidateCount() const;
    size_t activeAlertCount() const;
    uint32_t locationEpoch() const { return locationEpoch_; }

  private:
    Candidate* findMutable(uint32_t id);
    Candidate* allocate(uint32_t nowMs);
    uint32_t makeCandidateId(const Observation& observation) const;
    AlertLevel calculateLevel(const Candidate& candidate) const;
    uint32_t retentionMs(const Candidate& candidate) const;

    DetectionConfig config_;
    Candidate candidates_[kMaxCandidates];
    uint32_t locationEpoch_ = 0;
};

const char* alertLevelName(AlertLevel level);

} // namespace antistalker
