/*
 * ===========================================================================
 *  AntiStalker :: DetectionEngine.cpp
 *  See the header for the state machine. This is where the counting happens.
 * ===========================================================================
 */

#include "DetectionEngine.h"

#include <ctype.h>
#include <string.h>

namespace antistalker {
namespace {

constexpr uint32_t kMinuteMs = 60UL * 1000UL;

uint32_t fnv1a(const char* text, uint32_t hash = 2166136261UL) {
    while (*text != '\0') {
        const unsigned char c = static_cast<unsigned char>(*text++);
        hash ^= static_cast<uint8_t>(toupper(c));
        hash *= 16777619UL;
    }
    return hash;
}

void normalizeAddress(char* destination, size_t destinationSize, const char* source) {
    if (destinationSize == 0) {
        return;
    }

    size_t i = 0;
    for (; source != nullptr && source[i] != '\0' && i + 1 < destinationSize; ++i) {
        destination[i] = static_cast<char>(toupper(static_cast<unsigned char>(source[i])));
    }
    destination[i] = '\0';
}

uint32_t elapsed(uint32_t nowMs, uint32_t sinceMs) {
    // Unsigned subtraction survives millis() wrapping after ~49.7 days.
    // Write "now > since" instead and enjoy a bug report in 7 weeks.
    return nowMs - sinceMs;
}

} // namespace

DetectionEngine::DetectionEngine(DetectionConfig config) : config_(config) {}

EngineUpdate DetectionEngine::record(const Observation& observation) {
    EngineUpdate update;
    if (observation.match.family == TrackerFamily::None ||
        observation.address == nullptr || observation.address[0] == '\0') {
        return update;
    }

    update.accepted = true;
    update.id = makeCandidateId(observation);
    Candidate* candidate = findMutable(update.id);
    if (candidate == nullptr) {
        candidate = allocate(observation.nowMs);
        if (candidate == nullptr) {
            update.accepted = false;
            return update;
        }

        *candidate = Candidate();
        candidate->used = true;
        candidate->id = update.id;
        candidate->family = observation.match.family;
        candidate->firstSeenMs = observation.nowMs;
        candidate->lastSeenMs = observation.nowMs;
        candidate->filteredRssi = observation.rssi;
        candidate->strongestRssi = observation.rssi;
        candidate->distinctMinutes = 1;
        candidate->locations = 1;
        candidate->multiSightingLocations = 0;
        candidate->nearOwnerKnown = observation.match.nearOwnerKnown;
        candidate->nearOwner = observation.match.nearOwner;
        candidate->networkId = observation.match.networkId;
        candidate->lastEpoch = locationEpoch_;
        candidate->lastMinute = observation.nowMs / kMinuteMs;
        update.created = true;
    }

    normalizeAddress(candidate->address, sizeof(candidate->address), observation.address);
    candidate->addressType = observation.addressType;
    ++candidate->sightings;
    candidate->lastSeenMs = observation.nowMs;
    // Cheap low-pass filter (EMA, alpha = 1/4). Smooths the RSSI jitter
    // without floats.
    candidate->filteredRssi = (candidate->filteredRssi * 3 + observation.rssi) / 4;
    if (observation.rssi > candidate->strongestRssi) {
        candidate->strongestRssi = observation.rssi;
    }
    // Owner proximity describes THIS frame only, not history. Samsung states
    // 0/7 ("dunno") must wipe an earlier connected/offline reading.
    candidate->nearOwnerKnown = observation.match.nearOwnerKnown;
    candidate->nearOwner = observation.match.nearOwner;

    if (observation.trusted) {
        // Identity proven by the IRK: this is our own bonded device. Our own
        // phone following us around is called "having a phone". Allowed.
        candidate->trusted = true;
        candidate->allowed = true;
    }

    if (!update.created && candidate->lastEpoch != locationEpoch_) {
        candidate->lastEpoch = locationEpoch_;
        candidate->sightingsThisLocation = 0;
        if (candidate->locations < 0xFF) {
            ++candidate->locations;
        }
    }

    const uint32_t minute = observation.nowMs / kMinuteMs;
    if (candidate->lastMinute != minute) {
        candidate->lastMinute = minute;
        if (candidate->distinctMinutes < 0xFF) {
            ++candidate->distinctMinutes;
        }
    }

    if (candidate->sightingsThisLocation < 0xFF) {
        ++candidate->sightingsThisLocation;
    }
    if (candidate->sightingsThisLocation == 2 && candidate->multiSightingLocations < 0xFF) {
        ++candidate->multiSightingLocations;
    }

    const AlertLevel previous = candidate->level;
    candidate->level = calculateLevel(*candidate);
    update.previousLevel = previous;
    update.level = candidate->level;
    update.levelChanged = previous != candidate->level;
    update.alert = update.levelChanged &&
                   static_cast<uint8_t>(candidate->level) > static_cast<uint8_t>(previous) &&
                   candidate->level != AlertLevel::Nearby && !candidate->allowed;
    return update;
}

// New location epoch. Candidates notice lazily, on their next sighting.
void DetectionEngine::markLocationChanged() {
    ++locationEpoch_;
}

void DetectionEngine::prune(uint32_t nowMs) {
    for (Candidate& candidate : candidates_) {
        if (!candidate.used) {
            continue;
        }
        if (elapsed(nowMs, candidate.lastSeenMs) > retentionMs(candidate)) {
            candidate = Candidate();
        }
    }
}

uint32_t DetectionEngine::retentionMs(const Candidate& candidate) const {
    // VIP list (full 6 h): anything elevated, allowed, trusted, or already
    // seen at 2+ locations. That is evidence, we keep evidence.
    const bool significant = candidate.level != AlertLevel::Nearby || candidate.allowed ||
                             candidate.locations >= 2 || candidate.trusted;
    if (significant) {
        return config_.candidateExpiryMs;
    }
    // Everyone else: single-location noise. Few sightings -> 15 min of
    // silence and out. More sightings -> 1 h. The table is 64 slots, not a
    // museum.
    const uint32_t stale = candidate.sightings >= config_.minimumSightings
                               ? config_.staleActiveExpiryMs
                               : config_.staleNearbyExpiryMs;
    return stale < config_.candidateExpiryMs ? stale : config_.candidateExpiryMs;
}

void DetectionEngine::clear() {
    for (Candidate& candidate : candidates_) {
        candidate = Candidate();
    }
    locationEpoch_ = 0;
}

bool DetectionEngine::allow(uint32_t id, bool allowed) {
    Candidate* candidate = findMutable(id);
    if (candidate == nullptr) {
        return false;
    }
    if (candidate->trusted && !allowed) {
        // Trusted devices are allowed by their bond, not by the list. You
        // cannot 'unallow' your way out of a bond; remove the device instead.
        return true;
    }
    candidate->allowed = allowed;
    return true;
}

const Candidate* DetectionEngine::find(uint32_t id) const {
    for (const Candidate& candidate : candidates_) {
        if (candidate.used && candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

const Candidate* DetectionEngine::candidateAt(size_t index) const {
    return index < kMaxCandidates && candidates_[index].used ? &candidates_[index] : nullptr;
}

size_t DetectionEngine::candidateCount() const {
    size_t count = 0;
    for (const Candidate& candidate : candidates_) {
        count += candidate.used ? 1U : 0U;
    }
    return count;
}

size_t DetectionEngine::activeAlertCount() const {
    size_t count = 0;
    for (const Candidate& candidate : candidates_) {
        if (candidate.used && !candidate.allowed && candidate.level != AlertLevel::Nearby) {
            ++count;
        }
    }
    return count;
}

Candidate* DetectionEngine::findMutable(uint32_t id) {
    for (Candidate& candidate : candidates_) {
        if (candidate.used && candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

// Free slot if there is one, otherwise evict whoever was seen least recently.
Candidate* DetectionEngine::allocate(uint32_t nowMs) {
    Candidate* oldest = nullptr;
    for (Candidate& candidate : candidates_) {
        if (!candidate.used) {
            return &candidate;
        }
        if (oldest == nullptr || elapsed(nowMs, candidate.lastSeenMs) > elapsed(nowMs, oldest->lastSeenMs)) {
            oldest = &candidate;
        }
    }
    return oldest;
}

uint32_t DetectionEngine::makeCandidateId(const Observation& observation) const {
    if (observation.trusted) {
        // One candidate per bonded identity, whatever the frame looks like.
        // Your phone in a Continuity frame and your phone in a generic frame
        // is still one phone, not two.
        uint32_t hash = fnv1a(observation.address);
        hash ^= observation.addressType;
        hash *= 16777619UL;
        return hash;
    }

    uint32_t hash = 2166136261UL;
    const uint8_t family = static_cast<uint8_t>(observation.match.family);
    hash ^= family;
    hash *= 16777619UL;

    // Trackers with an identity payload: key on family + payload hash, so a
    // MAC rotation does not create a new candidate. Everyone else: MAC +
    // address type.
    if (observation.match.hasIdentityPayload) {
        for (size_t i = 0; i < sizeof(observation.match.identityHint); ++i) {
            hash ^= static_cast<uint8_t>(observation.match.identityHint >> (i * 8));
            hash *= 16777619UL;
        }
        return hash;
    }
    hash = fnv1a(observation.address, hash);
    hash ^= observation.addressType;
    hash *= 16777619UL;
    return hash;
}

AlertLevel DetectionEngine::calculateLevel(const Candidate& candidate) const {
    if (candidate.trusted) {
        return AlertLevel::Nearby; // Our own device. Of course it is here.
    }
    if (candidate.nearOwnerKnown && candidate.nearOwner) {
        // The tag says it is with its owner: probably a passenger's keys.
        // History is kept, so if it gets separated later it can escalate.
        return AlertLevel::Nearby;
    }

    const uint32_t duration = elapsed(candidate.lastSeenMs, candidate.firstSeenMs);
    if (candidate.locations >= 2 && candidate.multiSightingLocations >= 2 &&
        candidate.sightings >= config_.minimumSightings &&
        duration >= config_.followingMinDurationMs) {
        return AlertLevel::Following;
    }

    if (config_.enablePersistentState && candidate.sightings >= config_.minimumSightings &&
        candidate.distinctMinutes >= config_.minimumDistinctMinutes &&
        duration >= config_.persistentAfterMs) {
        return AlertLevel::Persistent;
    }
    return AlertLevel::Nearby;
}

const char* alertLevelName(AlertLevel level) {
    switch (level) {
        case AlertLevel::Following:
            return "FOLLOWING";
        case AlertLevel::Persistent:
            return "PERSISTENT";
        case AlertLevel::Nearby:
        default:
            return "NEARBY";
    }
}

} // namespace antistalker
