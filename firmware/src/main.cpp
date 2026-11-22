/*
 * ===========================================================================
 *  AntiStalker :: main.cpp
 *  The front desk of the asylum. Everybody checks in here.
 * ===========================================================================
 *
 *  What lives here:
 *    - the global singletons (two detection engines, the BLE scanner, the
 *      trust manager and the web portal)
 *    - setup(): boots Wi-Fi first, waits a second, THEN boots BLE
 *    - loop(): drains the BLE queue, babysits the pairing window, serves
 *      HTTP, reads the serial console and prunes old history once a minute
 *    - the serial command parser (type 'help', it is friendlier than it looks)
 *
 *  Why two engines?
 *    One engine watches tracking tags (AirTag & friends). The other watches
 *    "normal" stuff: phones, watches, earbuds, random BLE junk. They have
 *    different rules, because your neighbour's phone being near you all day
 *    is called "living in a building", not "being stalked".
 *
 *  Nothing in here talks to the internet. Ever. That is a feature.
 * ===========================================================================
 */

#include <Arduino.h>
#include <WiFi.h>

#include "BleMonitor.h"
#include "DetectionEngine.h"
#include "TrustManager.h"
#include "WebPortal.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace {

constexpr size_t kCommandCapacity = 96;
constexpr uint32_t kPruneIntervalMs = 60UL * 1000UL;

antistalker::DetectionConfig makeDeviceConfig() {
    antistalker::DetectionConfig config;
    // Your neighbour's phone sits behind the same wall as you for 14 hours a
    // day. If time alone could escalate ordinary devices, this thing would
    // scream all night. So: no PERSISTENT state for them. They only escalate
    // after showing up again at multiple locations YOU confirmed.
    config.enablePersistentState = false;
    return config;
}

antistalker::DetectionEngine trackerEngine;
antistalker::DetectionEngine deviceEngine(makeDeviceConfig());
BleMonitor bleMonitor;
TrustManager trustManager(bleMonitor);
WebPortal webPortal(trackerEngine, deviceEngine, bleMonitor, trustManager);
bool radioReady = false;
uint32_t lastPruneMs = 0;

void printHelp() {
    Serial.println(
        "Commands:\n"
        "  list                 Show tracker and personal/general BLE candidates\n"
        "  devices [on|off]     Show device candidates or toggle their monitoring\n"
        "  moved                Confirm arrival at a new physical location\n"
        "  allow <id>           Suppress alerts for your own device (RAM only)\n"
        "  unallow <id>         Re-enable alerts for a device\n"
        "  locate <id>          Show live RSSI for one candidate\n"
        "  locate off           Stop live RSSI output\n"
        "  verbose on|off       Show every BLE advertisement and raw payload\n"
        "  wifi [on|off]        Show network status or toggle Wi-Fi/web access\n"
        "  stats                Radio, packet, memory and alert statistics\n"
        "  clear                Clear observation history and location epochs\n"
        "  trustdump            Show bonded devices and resolving keys (debug)\n"
        "  help                 Show this help");
}

const char* displayState(const antistalker::Candidate& candidate) {
    if (antistalker::isPersonalDeviceFamily(candidate.family) &&
        candidate.level == antistalker::AlertLevel::Following) {
        return "CO-PRESENT";
    }
    return antistalker::alertLevelName(candidate.level);
}

void printCandidate(const antistalker::Candidate& candidate, uint32_t nowMs) {
    const char* owner = candidate.nearOwnerKnown ? (candidate.nearOwner ? "near" : "separated") : "unknown";
    Serial.printf(
        "id=%08lX type=%s state=%s%s%s mac=%s atype=%u rssi=%d peak=%d sightings=%lu minutes=%u locations=%u owner=%s age=%lus last=%lus\n",
        static_cast<unsigned long>(candidate.id), antistalker::trackerFamilyName(candidate.family),
        displayState(candidate),
        candidate.allowed ? " ALLOWED" : "", candidate.trusted ? " TRUSTED" : "", candidate.address,
        candidate.addressType, candidate.filteredRssi, candidate.strongestRssi,
        static_cast<unsigned long>(candidate.sightings), candidate.distinctMinutes,
        candidate.locations, owner, static_cast<unsigned long>((nowMs - candidate.firstSeenMs) / 1000UL),
        static_cast<unsigned long>((nowMs - candidate.lastSeenMs) / 1000UL));
    if (candidate.family == antistalker::TrackerFamily::Dult) {
        Serial.printf("      DULT network=0x%02X\n", candidate.networkId);
    }
}

size_t listEngine(const antistalker::DetectionEngine& engine, bool personalDevices) {
    size_t shown = 0;
    const uint32_t now = millis();
    for (size_t i = 0; i < antistalker::kMaxCandidates; ++i) {
        const antistalker::Candidate* candidate = engine.candidateAt(i);
        if (candidate != nullptr) {
            printCandidate(*candidate, now);
            ++shown;
        }
    }
    if (shown == 0) {
        Serial.println(personalDevices ? "No personal/general BLE candidates observed yet."
                                       : "No known tracker candidates observed yet.");
    } else {
        Serial.printf("%u %s candidate(s). IDs are observations, not people or owners.\n",
                      static_cast<unsigned>(shown), personalDevices ? "device" : "tracker");
    }
    return shown;
}

void listCandidates(bool includeTrackers = true, bool includeDevices = true) {
    if (includeTrackers) {
        Serial.println("--- Tracking tags ---");
        listEngine(trackerEngine, false);
    }
    if (includeDevices) {
        Serial.println("--- Phones / wearables / other BLE ---");
        listEngine(deviceEngine, true);
    }
}

bool parseId(const char* text, uint32_t& id) {
    if (text == nullptr || *text == '\0' || strlen(text) > 8) {
        return false;
    }
    for (const char* p = text; *p != '\0'; ++p) {
        if (!isxdigit(static_cast<unsigned char>(*p))) {
            return false;
        }
    }
    id = static_cast<uint32_t>(strtoul(text, nullptr, 16));
    return true;
}

const antistalker::Candidate* findCandidate(uint32_t id) {
    const antistalker::Candidate* candidate = trackerEngine.find(id);
    return candidate != nullptr ? candidate : deviceEngine.find(id);
}

void handleAllowCommand(char* idText, bool allowed) {
    uint32_t id;
    if (!parseId(idText, id)) {
        Serial.printf("Usage: %s <id>\n", allowed ? "allow" : "unallow");
        return;
    }
    const bool trackerFound = trackerEngine.allow(id, allowed);
    const bool deviceFound = deviceEngine.allow(id, allowed);
    if (!trackerFound && !deviceFound) {
        Serial.println("Candidate ID not found; run 'list' and use the current 8-digit ID.");
        return;
    }
    Serial.printf("%s %08lX\n", allowed ? "Allowed" : "Unallowed", static_cast<unsigned long>(id));
    const antistalker::Candidate* candidate = findCandidate(id);
    if (!allowed && candidate != nullptr && candidate->level != antistalker::AlertLevel::Nearby) {
        Serial.printf("[ALERT] %s id=%08lX (alerting re-enabled)\n", displayState(*candidate),
                      static_cast<unsigned long>(id));
    }
}

void handleLocateCommand(char* argument) {
    if (argument != nullptr && strcasecmp(argument, "off") == 0) {
        bleMonitor.stopLocating();
        Serial.println("Live RSSI stopped.");
        return;
    }

    uint32_t id;
    if (!parseId(argument, id) || findCandidate(id) == nullptr) {
        Serial.println("Usage: locate <id> (or 'locate off'); run 'list' for IDs.");
        return;
    }
    bleMonitor.setLocateId(id);
    Serial.printf("Locating %08lX. Move slowly; less-negative RSSI usually means closer, but is not distance.\n",
                  static_cast<unsigned long>(id));
}

void printStats() {
    Serial.printf(
        "radio=%s received=%lu tracker_packets=%lu device_packets=%lu dropped=%lu trackers=%u devices=%u alerts=%u location_epoch=%lu uptime=%lus\n",
        bleMonitor.scanning() ? "scanning" : "stopped",
        static_cast<unsigned long>(bleMonitor.receivedAdvertisements()),
        static_cast<unsigned long>(bleMonitor.matchedAdvertisements()),
        static_cast<unsigned long>(bleMonitor.personalAdvertisements()),
        static_cast<unsigned long>(bleMonitor.droppedAdvertisements()),
        static_cast<unsigned>(trackerEngine.candidateCount()), static_cast<unsigned>(deviceEngine.candidateCount()),
        static_cast<unsigned>(trackerEngine.activeAlertCount() + deviceEngine.activeAlertCount()),
        static_cast<unsigned long>(trackerEngine.locationEpoch()), millis() / 1000UL);
    Serial.printf("memory: free_heap=%u flash_free=%u psram=%u verbose=%s device_monitoring=%s\n",
                  ESP.getFreeHeap(), ESP.getFreeSketchSpace(), ESP.getFreePsram(),
                  bleMonitor.verbose() ? "on" : "off", bleMonitor.personalDevicesEnabled() ? "on" : "off");
    const float temperature = temperatureRead();
    if (!isnan(temperature)) {
        Serial.printf("chip_temperature=%.1fC\n", temperature);
    }
    const PhoneLocation& fix = webPortal.phoneLocation();
    if (fix.valid) {
        Serial.printf("phone_gps: lat=%.5f lon=%.5f accuracy=%ldm age=%lus\n", fix.latitude,
                      fix.longitude, lroundf(fix.accuracyM),
                      static_cast<unsigned long>((millis() - fix.receivedMs) / 1000UL));
    } else {
        Serial.println("phone_gps: no fix shared by the app yet");
    }
    Serial.printf("wifi=%s clients=%u\n", webPortal.running() ? "on" : "off",
                  webPortal.running() ? WiFi.softAPgetStationNum() : 0);
    Serial.printf("trust: paired=%u pairing=%s\n", static_cast<unsigned>(trustManager.deviceCount()),
                  trustManager.pairingActive() ? "open" : "off");
}

void handleDevicesCommand(char* argument) {
    if (argument == nullptr) {
        listCandidates(false, true);
        return;
    }
    if (strcasecmp(argument, "on") == 0 || strcasecmp(argument, "off") == 0) {
        bleMonitor.setPersonalDevicesEnabled(strcasecmp(argument, "on") == 0);
        Serial.printf("Personal/general BLE monitoring %s; existing history retained.\n",
                      bleMonitor.personalDevicesEnabled() ? "on" : "off");
    } else {
        Serial.println("Usage: devices [on|off]");
    }
}

void handleWifiCommand(char* argument) {
    if (argument == nullptr) {
        if (webPortal.running()) {
            webPortal.printCredentials();
        } else {
            Serial.println("Wi-Fi and local web access are off; use 'wifi on'.");
        }
        return;
    }
    if (strcasecmp(argument, "off") == 0) {
        webPortal.stop();
        Serial.println("Wi-Fi and local web access stopped; BLE monitoring continues.");
    } else if (strcasecmp(argument, "on") == 0) {
        if (webPortal.begin()) {
            webPortal.printCredentials();
        } else {
            Serial.println("Failed to start Wi-Fi; BLE monitoring continues.");
        }
    } else {
        Serial.println("Usage: wifi [on|off]");
    }
}

void processCommand(char* line) {
    char* command = strtok(line, " \t");
    if (command == nullptr) {
        return;
    }
    char* argument = strtok(nullptr, " \t");

    if (strcasecmp(command, "help") == 0) {
        printHelp();
    } else if (strcasecmp(command, "list") == 0) {
        listCandidates();
    } else if (strcasecmp(command, "devices") == 0) {
        handleDevicesCommand(argument);
    } else if (strcasecmp(command, "moved") == 0) {
        trackerEngine.markLocationChanged();
        deviceEngine.markLocationChanged();
        Serial.printf("Location epoch %lu confirmed. Keep scanning; two sightings at the new location are required.\n",
                      static_cast<unsigned long>(trackerEngine.locationEpoch()));
    } else if (strcasecmp(command, "allow") == 0) {
        handleAllowCommand(argument, true);
    } else if (strcasecmp(command, "unallow") == 0) {
        handleAllowCommand(argument, false);
    } else if (strcasecmp(command, "locate") == 0) {
        handleLocateCommand(argument);
    } else if (strcasecmp(command, "verbose") == 0) {
        if (argument != nullptr && (strcasecmp(argument, "on") == 0 || strcasecmp(argument, "off") == 0)) {
            bleMonitor.setVerbose(strcasecmp(argument, "on") == 0);
            Serial.printf("Verbose mode %s.\n", bleMonitor.verbose() ? "on" : "off");
        } else {
            Serial.println("Usage: verbose on|off");
        }
    } else if (strcasecmp(command, "wifi") == 0) {
        handleWifiCommand(argument);
    } else if (strcasecmp(command, "trustdump") == 0) {
        // Owner-only debug dump. Yes, it prints the IRKs in plain hex.
        // That is fine ONLY because this is the physical serial port: if you
        // can type here you already have the board in your hands. These keys
        // never, ever go out through the web API. Do not "improve" that.
        Serial.println("Trusted identities (serial console only):");
        for (size_t i = 0; i < antistalker::TrustResolver::kMaxIdentities; ++i) {
            antistalker::TrustIdentity identity;
            if (!trustManager.deviceAt(i, identity)) {
                continue;
            }
            char formatted[18];
            antistalker::TrustResolver::formatAddress(identity.address, formatted, sizeof(formatted));
            Serial.printf("  %s type=%u resolved=%lu irk=", formatted, identity.addressType,
                          static_cast<unsigned long>(identity.resolvedCount));
            for (size_t b = 0; b < sizeof(identity.irk); ++b) {
                Serial.printf("%02x", identity.irk[b]);
            }
            Serial.printf(" name=%s\n", identity.name);
        }
        Serial.printf("Own BLE address: %s; NimBLE bonds: %d\n",
                      NimBLEDevice::getAddress().toString().c_str(),
                      NimBLEDevice::getNumBonds());
        for (int i = 0; i < NimBLEDevice::getNumBonds(); ++i) {
            Serial.printf("  bond[%d] = %s\n", i,
                           NimBLEDevice::getBondedAddress(i).toString().c_str());
        }
    } else if (strcasecmp(command, "stats") == 0) {
        printStats();
    } else if (strcasecmp(command, "clear") == 0) {
        trackerEngine.clear();
        deviceEngine.clear();
        bleMonitor.stopLocating();
        Serial.println("Observation history and allow lists cleared; Wi-Fi credentials retained.");
    } else {
        Serial.println("Unknown command. Type 'help'.");
    }
}

// Line-buffered, non-blocking serial reader. If you paste a novel into the
// console it gets dropped with "Command too long." instead of smashing the
// buffer. 96 chars is plenty for "allow DEADBEEF".
void pollSerial() {
    static char commandLine[kCommandCapacity];
    static size_t length = 0;
    static bool overflow = false;

    while (Serial.available() > 0) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (overflow) {
                Serial.println("Command too long.");
            } else if (length > 0) {
                commandLine[length] = '\0';
                processCommand(commandLine);
            }
            length = 0;
            overflow = false;
        } else if (!overflow) {
            if (length + 1 < kCommandCapacity) {
                commandLine[length++] = c;
            } else {
                overflow = true;
                length = 0;
            }
        }
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    // 80 MHz is plenty for parsing adverts and serving JSON. Running both
    // radios at 240 MHz from a cheap USB cable is how you meet BROWNOUT_RST.
    setCpuFrequencyMhz(80);
    const uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 1500) {
        delay(10);
    }

    Serial.println("\nAntiStalker 0.5.0 - passive BLE tracker and co-presence monitor");
    Serial.println("ESP32-S3 detects BLE only; nearby/persistent/following/co-present are risk signals, not accusations.");
    Serial.printf("Hardware: flash=%u bytes psram=%u bytes\n", ESP.getFlashChipSize(), ESP.getPsramSize());
    if (ESP.getFlashChipSize() != 16UL * 1024UL * 1024UL || ESP.getPsramSize() == 0) {
        Serial.println("[WARN] Expected N16R8 flash/PSRAM was not detected; check board and memory settings.");
    }

    Serial.println("[BOOT] Starting Wi-Fi and local dashboard...");
    if (webPortal.begin()) {
        webPortal.printCredentials();
        Serial.println("[WIFI] The fallback hotspot stops while home Wi-Fi is connected.");
    } else {
        Serial.println("[WIFI] Wi-Fi failed to start; use 'wifi on' to retry.");
    }
    // Wi-Fi and BLE share ONE 2.4 GHz radio. Starting both at the same
    // instant makes a nice current spike, and the spike makes a nice reboot.
    // One second of patience fixes it. Cheapest bug fix in the whole project.
    delay(1000);

    Serial.println("[BOOT] Starting BLE controller...");
    radioReady = bleMonitor.begin();
    Serial.println(radioReady ? "[RADIO] Continuous passive BLE scan started."
                              : "[RADIO] BLE initialization failed; scanner disabled until reset.");
    // Load the IRKs of devices you paired before, so their rotating addresses
    // get recognised passively. On a fresh board this loads nothing at all.
    trustManager.begin();
    bleMonitor.setTrustResolver(trustManager.resolver());
    Serial.println("Run 'moved' after genuinely changing location. Type 'help' for commands.");
    lastPruneMs = millis();
}

void loop() {
    if (radioReady) {
        bleMonitor.poll(trackerEngine, deviceEngine);
    }
    trustManager.poll(); // Closes the pairing window on timeout.
    webPortal.handle();
    pollSerial();

    // Once a minute, throw away history nobody cares about anymore.
    // See DetectionEngine::retentionMs() for who gets kicked out first.
    const uint32_t now = millis();
    if (now - lastPruneMs >= kPruneIntervalMs) {
        trackerEngine.prune(now);
        deviceEngine.prune(now);
        lastPruneMs = now;
    }
    delay(2);
}
