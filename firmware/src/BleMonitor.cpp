/*
 * ===========================================================================
 *  AntiStalker :: BleMonitor.cpp
 *  Scanner setup, the radio-side callback, and the loop-side dispatcher.
 * ===========================================================================
 *
 *  Read BleMonitor.h first for the big picture (queue, two engines, etc).
 *
 *  Everything printed to Serial from here is written very carefully to say
 *  "this is a radio observation" and never "this is a stalker". A BLE advert
 *  is a few bytes of unauthenticated text shouted into the air. Treat it
 *  that way.
 * ===========================================================================
 */

#include "BleMonitor.h"

#include "PersonalDeviceClassifier.h"
#include "TrackerClassifier.h"
#include "TrustResolver.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

template <size_t N>
void copyText(char (&destination)[N], const std::string& source) {
    const size_t count = std::min(N - 1, source.size());
    memcpy(destination, source.data(), count);
    destination[count] = '\0';
}

template <size_t N>
void copyName(char (&destination)[N], const std::string& source) {
    // BLE names are untrusted input. Anyone can call their device
    // "\x1b[2J\x1b[31mYOU ARE HACKED" and wipe your terminal for fun.
    // So: printable ASCII survives, everything else becomes a visible '.'.
    size_t output = 0;
    for (size_t i = 0; i < source.size() && output + 1 < N; ++i) {
        const uint8_t c = static_cast<uint8_t>(source[i]);
        destination[output++] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.';
    }
    destination[output] = '\0';
}

const char* ownerState(const antistalker::Candidate& candidate) {
    if (!candidate.nearOwnerKnown) {
        return "unknown";
    }
    return candidate.nearOwner ? "near" : "separated";
}

} // namespace

bool BleMonitor::begin() {
    queue_ = xQueueCreate(kQueueDepth, sizeof(AdvertisementEvent));
    if (queue_ == nullptr) {
        return false;
    }

    if (!NimBLEDevice::init("AntiStalker")) {
        return false;
    }
    scan_ = NimBLEDevice::getScan();
    if (scan_ == nullptr) {
        return false;
    }

    scan_->setScanCallbacks(this, true); // true = report duplicates. Repeats ARE the signal.
    scan_->setActiveScan(false);        // Passive. We listen, we never ask.
    scan_->setMaxResults(0);            // Callback only; NimBLE keeps no result list.
    // 50% duty cycle: listen 80 ms out of every 160 ms. The other half of the
    // airtime goes to Wi-Fi so the dashboard does not feel like dial-up.
    scan_->setInterval(160);            // milliseconds
    scan_->setWindow(80);               // milliseconds
#if defined(CONFIG_BT_NIMBLE_EXT_ADV) && CONFIG_BT_NIMBLE_EXT_ADV
    scan_->setPhy(NimBLEScan::SCAN_ALL); // BLE 5 extended: 1M + Coded PHY.
#endif
    return scan_->start(0, false);      // duration 0 = scan continuously
}

void BleMonitor::poll(antistalker::DetectionEngine& trackerEngine,
                      antistalker::DetectionEngine& deviceEngine) {
    if (restartRequested_) {
        restartRequested_ = false;
        if (scanningEnabled_ && scan_ != nullptr && !scan_->isScanning() &&
            !scan_->start(0, true)) {
            Serial.println("[RADIO] Failed to restart BLE scan; reset the device if this persists.");
        }
    }

    if (queue_ == nullptr) {
        return;
    }

    // At most 16 events per loop() pass. In a packed metro carriage the queue
    // never empties, and without a cap the serial console and the web server
    // would starve. Whatever is left waits for the next pass.
    for (size_t i = 0; i < 16; ++i) {
        AdvertisementEvent event;
        if (xQueueReceive(queue_, &event, 0) != pdTRUE) {
            break;
        }
        processEvent(event, trackerEngine, deviceEngine);
    }
}

void BleMonitor::setScanningEnabled(bool enabled) {
    scanningEnabled_ = enabled;
    if (scan_ == nullptr) {
        return;
    }
    if (!enabled && scan_->isScanning()) {
        scan_->stop(); // onScanEnd() will not auto-restart while disabled.
    } else if (enabled && !scan_->isScanning() && !scan_->start(0, true)) {
        Serial.println("[RADIO] Failed to restart BLE scan; reset the device if this persists.");
    }
}

// Runs in the NimBLE host task, NOT in loop(). Rules for this function:
// copy, enqueue, leave. No parsing, no printing, no blocking. If the queue is
// full we drop the advert and count it, because blocking here would stall the
// whole BLE stack and that is worse than losing one packet.
void BleMonitor::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    if (queue_ == nullptr || advertisedDevice == nullptr) {
        return;
    }

    ++receivedAdvertisements_;
    AdvertisementEvent event = {};
    copyText(event.address, advertisedDevice->getAddress().toString());
    if (advertisedDevice->haveName()) {
        copyName(event.name, advertisedDevice->getName());
    }
    event.addressType = advertisedDevice->getAddressType();
    event.rssi = advertisedDevice->getRSSI();
    event.nowMs = millis();

    const std::vector<uint8_t>& payload = advertisedDevice->getPayload();
    event.payloadLength = static_cast<uint16_t>(std::min(payload.size(), kMaxPayloadBytes));
    if (event.payloadLength > 0) {
        memcpy(event.payload, payload.data(), event.payloadLength);
    }

    if (xQueueSend(queue_, &event, 0) != pdTRUE) {
        ++droppedAdvertisements_;
    }
}

void BleMonitor::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    (void)scanResults;
    (void)reason;
    // A "continuous" scan can still end (controller hiccup, coexistence
    // drama). We do not restart from inside the callback; we raise a flag and
    // let poll() restart it from loop() context.
    restartRequested_ = true;
}

void BleMonitor::processEvent(const AdvertisementEvent& event,
                              antistalker::DetectionEngine& trackerEngine,
                              antistalker::DetectionEngine& deviceEngine) {
    using namespace antistalker;

    // Step 1: is this one of OUR devices in disguise? Bonded phones/watches
    // hide behind rotating resolvable private addresses (RPAs). If one of the
    // stored IRKs resolves it, swap the throwaway address for the real
    // identity address before anything else sees it.
    const AdvertisementEvent* observed = &event;
    AdvertisementEvent resolvedEvent;
    if (trust_ != nullptr && event.addressType == TrustResolver::kRandomAddressType) {
        uint8_t rpa[6];
        if (TrustResolver::parseAddress(event.address, rpa) &&
            TrustResolver::isResolvablePrivateAddress(rpa)) {
            TrustIdentity* identity = trust_->resolve(rpa, event.name);
            if (identity != nullptr) {
                resolvedEvent = event;
                TrustResolver::formatAddress(identity->address, resolvedEvent.address,
                                             sizeof(resolvedEvent.address));
                resolvedEvent.addressType = identity->addressType;
                resolvedEvent.trusted = true;
                observed = &resolvedEvent;
            }
        }
    }

    // Step 2: classify. Tracker signatures win; only if it is NOT a known tag
    // do we ask the personal-device classifier what it might be.
    const TrackerMatch trackerMatch =
        TrackerClassifier::classify(observed->payload, observed->payloadLength);
    const TrackerMatch personalMatch = trackerMatch.family == TrackerFamily::None
                                           ? PersonalDeviceClassifier::classify(observed->payload, observed->payloadLength)
                                           : TrackerMatch();
    if (verbose_) {
        printVerbose(*observed, trackerMatch.family != TrackerFamily::None ? trackerMatch.family
                                                                       : personalMatch.family);
    }

    if (trackerMatch.family != TrackerFamily::None) {
        ++matchedAdvertisements_;
        processUpdate(*observed, trackerMatch, trackerEngine, false);
    } else if (observed->trusted) {
        // Our own bonded device: always recorded, even with 'devices off' and
        // even if the frame is unclassifiable garbage. You want to see your
        // own phone on the dashboard, it is reassuring.
        ++personalAdvertisements_;
        TrackerMatch trustedMatch;
        trustedMatch.family = personalMatch.family != TrackerFamily::None
                                  ? personalMatch.family
                                  : TrackerFamily::GenericBle;
        processUpdate(*observed, trustedMatch, deviceEngine, true);
    } else if (personalDevicesEnabled_ && personalMatch.family != TrackerFamily::None) {
        ++personalAdvertisements_;
        processUpdate(*observed, personalMatch, deviceEngine, true);
    }
}

// Feeds one observation to an engine and prints whatever changed. Note the
// wording of every message: "observed", "not proof", "check your own devices
// first". The firmware is allowed to be paranoid. It is not allowed to accuse.
void BleMonitor::processUpdate(const AdvertisementEvent& event, const antistalker::TrackerMatch& match,
                               antistalker::DetectionEngine& engine, bool personalDevice) {
    using namespace antistalker;
    Observation observation;
    observation.address = event.address;
    observation.addressType = event.addressType;
    observation.rssi = event.rssi;
    observation.nowMs = event.nowMs;
    observation.trusted = event.trusted;
    observation.match = match;
    const EngineUpdate update = engine.record(observation);
    if (!update.accepted) {
        return;
    }
    const Candidate* candidate = engine.find(update.id);
    if (candidate == nullptr) {
        return;
    }

    if (update.created && candidate->trusted) {
        const char* trustName = candidate->address;
        if (trust_ != nullptr) {
            uint8_t address[6];
            if (antistalker::TrustResolver::parseAddress(candidate->address, address)) {
                const antistalker::TrustIdentity* identity =
                    trust_->findIdentity(address, candidate->addressType);
                if (identity != nullptr && identity->name[0] != '\0') {
                    trustName = identity->name;
                }
            }
        }
        Serial.printf("[TRUST] Bonded device present: %s (identity address %s, rssi=%d).\n",
                      trustName, candidate->address, candidate->filteredRssi);
    }

    if (update.created && !candidate->allowed) {
        if (!personalDevice) {
            Serial.printf("[NEW] id=%08lX type=%s mac=%s rssi=%d owner=%s\n",
                          static_cast<unsigned long>(candidate->id), trackerFamilyName(candidate->family),
                          candidate->address, candidate->filteredRssi, ownerState(*candidate));
            Serial.println("      This is a nearby tracker-capable device, not proof of stalking.");
        } else if (candidate->family != TrackerFamily::GenericBle) {
            Serial.printf("[DEVICE] id=%08lX type=%s mac=%s rssi=%d\n",
                          static_cast<unsigned long>(candidate->id), trackerFamilyName(candidate->family),
                          candidate->address, candidate->filteredRssi);
            Serial.println("      Ordinary device detected; co-presence alone is not proof of stalking.");
        }
    }

    if (update.levelChanged && !candidate->allowed) {
        if (update.alert && personalDevice) {
            Serial.printf("[ALERT] CO-PRESENT id=%08lX type=%s sightings=%lu locations=%u rssi=%d\n",
                          static_cast<unsigned long>(candidate->id), trackerFamilyName(candidate->family),
                          static_cast<unsigned long>(candidate->sightings), candidate->locations,
                          candidate->filteredRssi);
            Serial.println("      Same MAC/address type repeated at multiple user-confirmed locations.");
            Serial.println("      This is not identity or intent. Check for your own/shared devices first.");
        } else if (update.alert) {
            Serial.printf("[ALERT] %s id=%08lX type=%s sightings=%lu locations=%u rssi=%d\n",
                          alertLevelName(candidate->level), static_cast<unsigned long>(candidate->id),
                          trackerFamilyName(candidate->family),
                          static_cast<unsigned long>(candidate->sightings), candidate->locations,
                          candidate->filteredRssi);
            if (candidate->level == AlertLevel::Following) {
                Serial.println("      Same payload identity observed at multiple user-confirmed locations.");
                Serial.println("      Use 'locate <id>' for relative RSSI and check your belongings safely.");
            } else {
                Serial.println("      Long-lived nearby signal; confirm with movement before treating as suspicious.");
            }
        } else {
            Serial.printf("[%s] id=%08lX now=%s owner=%s\n", personalDevice ? "DEVICE-STATE" : "STATE",
                          static_cast<unsigned long>(candidate->id), alertLevelName(candidate->level),
                          ownerState(*candidate));
        }
    }

    // 'locate <id>' mode: a poor man's metal detector. Capped at 2 lines per
    // second so the terminal stays readable while you wave the board around.
    if (locating_ && locateId_ == candidate->id &&
        static_cast<uint32_t>(event.nowMs - lastLocatePrintMs_) >= 500) {
        lastLocatePrintMs_ = event.nowMs;
        Serial.printf("[RSSI] id=%08lX current=%d filtered=%d strongest=%d\n",
                      static_cast<unsigned long>(candidate->id), event.rssi,
                      candidate->filteredRssi, candidate->strongestRssi);
    }
}

void BleMonitor::printVerbose(const AdvertisementEvent& event,
                              antistalker::TrackerFamily family) const {
    Serial.printf("[BLE] mac=%s rssi=%d type=%s%s", event.address, event.rssi,
                  antistalker::trackerFamilyName(family), event.trusted ? " TRUSTED" : "");
    if (event.name[0] != '\0') {
        Serial.printf(" name=%s", event.name);
    }
    Serial.print(" data=");
    for (size_t i = 0; i < event.payloadLength; ++i) {
        Serial.printf("%02X", event.payload[i]);
    }
    Serial.println();
}

void BleMonitor::setLocateId(uint32_t id) {
    locateId_ = id;
    locating_ = true;
    lastLocatePrintMs_ = 0;
}

void BleMonitor::stopLocating() {
    locating_ = false;
    locateId_ = 0;
}

bool BleMonitor::scanning() {
    return scan_ != nullptr && scan_->isScanning();
}
