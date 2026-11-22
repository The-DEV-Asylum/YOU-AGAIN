/*
 * ===========================================================================
 *  AntiStalker :: TrustManager.cpp
 *  Pairing window, IRK capture and NVS persistence. See the header for the
 *  flow diagram, this file is the plumbing.
 * ===========================================================================
 */

#include "TrustManager.h"

#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/host/include/host/ble_store.h"

#include <Preferences.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t kDefaultPairingSeconds = 300;
constexpr uint32_t kMaxPairingSeconds = 900;

// NVS blob layout for one trusted identity. Stored as raw bytes under keys
// "id0".."id3". If you change this struct, old blobs fail the size check in
// begin() and get ignored (i.e. users re-pair). The static_assert is there
// so you do not change it by accident.
struct StoredIdentity {
    char name[25];
    uint8_t irk[16];
    uint8_t address[6];
    uint8_t addressType;
};
static_assert(sizeof(StoredIdentity) == 48, "stored trust entry must stay packed");

} // namespace

TrustManager::TrustManager(BleMonitor& monitor) : monitor_(monitor) {}

bool TrustManager::begin() {
    Preferences preferences;
    if (!preferences.begin("trust", true)) {
        return false;
    }
    for (size_t i = 0; i < antistalker::TrustResolver::kMaxIdentities; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "id%u", static_cast<unsigned>(i));
        StoredIdentity stored;
        if (preferences.getBytesLength(key) != sizeof(stored)) {
            continue;
        }
        preferences.getBytes(key, &stored, sizeof(stored));
        resolver_.addIdentity(stored.name, stored.irk, stored.address, stored.addressType);
    }
    preferences.end();
    if (resolver_.count() > 0) {
        Serial.printf("[TRUST] %u trusted device(s) loaded; their rotating addresses will be "
                      "recognized passively.\n",
                      static_cast<unsigned>(resolver_.count()));
    }
    return true;
}

bool TrustManager::startPairing(uint32_t seconds, uint32_t& passkeyOut) {
    if (pairing_) {
        passkeyOut = passkey_;
        return true; // Window already open; keep its original expiry.
    }
    if (seconds == 0 || seconds > kMaxPairingSeconds) {
        seconds = kDefaultPairingSeconds;
    }

    // NimBLE only lets you build the GATT database while no GAP procedure is
    // running (ble_gatts_mutable()). A scan IS a GAP procedure. So the scan
    // has to stop before the pairing server starts. The window is short and
    // rare, and detection resumes right after.
    monitor_.setScanningEnabled(false);
    delay(20); // Let the scan-cancel command reach the controller.

    if (server_ == nullptr) {
        server_ = NimBLEDevice::createServer();
        if (server_ == nullptr) {
            monitor_.setScanningEnabled(true);
            return false;
        }
        server_->setCallbacks(this, false);
    }

    // Bond + MITM protection + LE Secure Connections. The passkey comes from
    // the hardware RNG and is shown only on the owner's screen. No code, no
    // bond. "Just Works" pairing would have been easier and also useless.
    NimBLEDevice::setSecurityAuth(true /*bond*/, true /*mitm*/, true /*secure connections*/);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY); // We display, they enter.
    passkey_ = esp_random() % 1000000UL;
    NimBLEDevice::setSecurityPasskey(passkey_);
    passkeyOut = passkey_;

    snprintf(advertisingName_, sizeof(advertisingName_), "AntiStalker-%04X",
             static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFFU));
    NimBLEExtAdvertising* advertising = NimBLEDevice::getAdvertising();
    NimBLEExtAdvertisement data;
    // Legacy adverts, because phone Bluetooth settings screens mostly ignore
    // fancy BLE 5 extended ones.
    data.setLegacyAdvertising(true);
    data.setConnectable(true);
    data.setScannable(true);
    // iOS fills its Bluetooth list at the speed of a sleepy sloth. Advertise
    // every 30-60 ms (units of 0.625 ms) while the window is open so the
    // entry shows up in seconds instead of "go make a coffee".
    data.setMinInterval(0x30);
    data.setMaxInterval(0x60);
    data.setFlags(0x06);             // General discoverable; BR/EDR not supported.
    data.setName(advertisingName_);
    if (!advertising->setInstanceData(0, data) || !advertising->start(0, 0)) {
        Serial.println("[TRUST] Failed to start the pairing advertisement; try again.");
        monitor_.setScanningEnabled(true);
        return false;
    }

    pairing_ = true;
    pairingExpiresMs_ = millis() + seconds * 1000UL;
    Serial.printf("[TRUST] Pairing window open for %lu s. On the phone: Settings > Bluetooth > "
                  "tap '%s', then enter code %06lu.\n",
                  static_cast<unsigned long>(seconds), advertisingName_,
                  static_cast<unsigned long>(passkey_));
    return true;
}

void TrustManager::cancelPairing() {
    if (!pairing_) {
        return;
    }
    NimBLEDevice::getAdvertising()->stop(0);
    pairing_ = false;
    monitor_.setScanningEnabled(true);
    Serial.println("[TRUST] Pairing window closed; passive scanning resumed.");
}

uint32_t TrustManager::pairingRemainingS() const {
    if (!pairing_) {
        return 0;
    }
    const int32_t remainingMs = static_cast<int32_t>(pairingExpiresMs_ - millis());
    return remainingMs > 0 ? static_cast<uint32_t>(remainingMs) / 1000UL + 1U : 0U;
}

void TrustManager::poll() {
    if (pairing_ && static_cast<int32_t>(millis() - pairingExpiresMs_) >= 0) {
        Serial.println("[TRUST] Pairing window timed out.");
        cancelPairing();
    }
}

bool TrustManager::deviceAt(size_t index, antistalker::TrustIdentity& out) const {
    const antistalker::TrustIdentity* identity = resolver_.identityAt(index);
    if (identity == nullptr || !identity->used) {
        return false;
    }
    out = *identity;
    return true;
}

bool TrustManager::removeDevice(const char* identityText) {
    uint8_t address[6];
    if (!antistalker::TrustResolver::parseAddress(identityText, address)) {
        return false;
    }
    for (uint8_t type = antistalker::TrustResolver::kPublicAddressType;
         type <= antistalker::TrustResolver::kRandomAddressType; ++type) {
        if (resolver_.findIdentity(address, type) == nullptr) {
            continue;
        }
        resolver_.removeIdentity(address, type);
        char formatted[18];
        antistalker::TrustResolver::formatAddress(address, formatted, sizeof(formatted));
        NimBLEDevice::deleteBond(NimBLEAddress(formatted, type));
        if (!persist()) {
            Serial.println("[TRUST] Warning: could not update trust storage.");
        }
        Serial.printf("[TRUST] Trusted device %s removed.\n", formatted);
        return true;
    }
    return false;
}

bool TrustManager::renameDevice(const char* identityText, const char* name) {
    uint8_t address[6];
    if (!antistalker::TrustResolver::parseAddress(identityText, address) || name == nullptr ||
        name[0] == '\0') {
        return false;
    }
    for (uint8_t type = antistalker::TrustResolver::kPublicAddressType;
         type <= antistalker::TrustResolver::kRandomAddressType; ++type) {
        if (resolver_.findIdentity(address, type) == nullptr) {
            continue;
        }
        if (resolver_.setName(address, type, name) && persist()) {
            return true;
        }
    }
    return false;
}

bool TrustManager::persist() {
    Preferences preferences;
    if (!preferences.begin("trust", false)) {
        return false;
    }
    for (size_t i = 0; i < antistalker::TrustResolver::kMaxIdentities; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "id%u", static_cast<unsigned>(i));
        const antistalker::TrustIdentity* identity = resolver_.identityAt(i);
        if (identity == nullptr || !identity->used) {
            preferences.remove(key);
            continue;
        }
        StoredIdentity stored;
        memset(&stored, 0, sizeof(stored));
        strncpy(stored.name, identity->name, sizeof(stored.name) - 1);
        memcpy(stored.irk, identity->irk, sizeof(stored.irk));
        memcpy(stored.address, identity->address, sizeof(stored.address));
        stored.addressType = identity->addressType;
        preferences.putBytes(key, &stored, sizeof(stored));
    }
    preferences.end();
    return true;
}

void TrustManager::onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) {
    (void)server;
    Serial.printf("[TRUST] %s connected; requesting secure pairing.\n",
                  connInfo.getAddress().toString().c_str());
    // Plot twist discovered in the field: phones mostly use their IRK-based
    // rotating address only when CONNECTING. Their idle "find my" beacons
    // rotate with separate vendor schemes that have nothing to do with the
    // IRK. So a connection is the most reliable place to recognise a
    // trusted phone. Check it here too.
    {
        // getVal() is little-endian; the resolver works in display order
        // (most significant byte first). Flip it.
        const uint8_t* value = connInfo.getAddress().getVal();
        uint8_t peer[6];
        for (size_t i = 0; i < 6; ++i) {
            peer[i] = value[5 - i];
        }
        if (antistalker::TrustIdentity* identity = resolver_.resolve(peer, nullptr)) {
            Serial.printf("[TRUST] Recognized %s: rotating address of trusted '%s'.\n",
                          connInfo.getAddress().toString().c_str(), identity->name);
        }
    }
    if (!NimBLEDevice::startSecurity(connInfo.getConnHandle())) {
        Serial.println("[TRUST] Could not send the security request; the central must pair "
                       "on its own.");
    }
}

void TrustManager::onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) {
    (void)server;
    // The HCI reason code is the whole pairing story in one byte:
    //   0x06 PIN/key missing, 0x07 wrong passkey (fat fingers),
    //   0x13 user cancelled (changed their mind), and so on.
    Serial.printf("[TRUST] %s disconnected; reason=0x%02x%s.\n",
                  connInfo.getAddress().toString().c_str(), reason,
                  pairing_ ? " (pairing window still open)" : "");
}

uint32_t TrustManager::onPassKeyDisplay() {
    Serial.printf("[TRUST] Showing passkey %06lu to the pairing device.\n",
                  static_cast<unsigned long>(passkey_));
    return passkey_;
}

void TrustManager::onAuthenticationComplete(NimBLEConnInfo& connInfo) {
    const NimBLEAddress identity = connInfo.getIdAddress();

    // By now NimBLE has written the bond to its store, including the peer's
    // IRK and identity address. Go fetch it. Some phones "tap-pair" without
    // distributing an IRK; those get politely ignored and the window stays
    // open so the user can try again.
    struct ble_store_key_sec key = {};
    key.peer_addr = *identity.getBase();
    struct ble_store_value_sec sec = {};
    const int rc = ble_store_read_peer_sec(&key, &sec);
    Serial.printf("[TRUST] Pairing finished for %s (type %d): store lookup rc=%d, IRK=%d.\n",
                  identity.toString().c_str(), identity.getType(), rc,
                  sec.irk_present ? 1 : 0);
    if (rc != 0 || !sec.irk_present) {
        Serial.printf("[TRUST] Pairing with %s shared no identity-resolving key; window stays "
                      "open.\n",
                      identity.toString().c_str());
        return;
    }

    uint8_t address[6];
    const uint8_t* value = identity.getVal(); // Little-endian; reverse to display order.
    for (size_t i = 0; i < 6; ++i) {
        address[i] = value[5 - i];
    }
    char name[25];
    snprintf(name, sizeof(name), "Trusted %u", static_cast<unsigned>(resolver_.count() + 1));
    if (!resolver_.addIdentity(name, sec.irk, address, identity.getType())) {
        Serial.println("[TRUST] Trust table is full; remove a device first.");
        return;
    }
    if (!persist()) {
        Serial.println("[TRUST] Warning: could not write trust storage.");
    }
    Serial.printf("[TRUST] Paired device saved: identity=%s (rename it in the app or dashboard); "
                  "its rotating addresses are now recognized.\n",
                  identity.toString().c_str());
    server_->disconnect(connInfo);
    cancelPairing();
}
