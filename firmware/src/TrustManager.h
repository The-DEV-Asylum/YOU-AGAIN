#pragma once

#include "BleMonitor.h"
#include "TrustResolver.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

/*
 * ===========================================================================
 *  AntiStalker :: TrustManager.h
 *  "Hi, it's me, your own phone. Please stop panicking about me."
 * ===========================================================================
 *
 *  The one and only time this firmware stops being purely passive.
 *
 *  The flow:
 *
 *      user taps "pair" in app/dashboard
 *              |
 *              V
 *      passive scan STOPS, board advertises "AntiStalker-XXXX" (connectable)
 *              |
 *              V
 *      phone: Settings > Bluetooth > AntiStalker-XXXX > types 6-digit code
 *              |    (board is "display only": we show it, they type it)
 *              V
 *      bonding hands us the phone's IRK + real identity address
 *              |
 *              V
 *      saved to NVS, advert stops, passive scan RESUMES
 *              |
 *              V
 *      forever after: every rotating address that phone transmits is
 *      matched offline with the spec's ah() function. No connection needed.
 *
 *  Security bit: a stranger standing next to you cannot mark themselves as
 *  trusted. Without the passkey that only appears on YOUR screen, the MITM-
 *  protected pairing simply fails. And the window closes by itself.
 * ===========================================================================
 */
class TrustManager : private NimBLEServerCallbacks {
  public:
    explicit TrustManager(BleMonitor& monitor);

    // Loads paired identities from NVS. Call after NimBLE initialization.
    bool begin();

    antistalker::TrustResolver& resolver() { return resolver_; }
    const antistalker::TrustResolver& resolver() const { return resolver_; }

    // Opens (or keeps open) the pairing window. passkeyOut receives the code
    // the owner must enter on the device being paired.
    bool startPairing(uint32_t seconds, uint32_t& passkeyOut);
    void cancelPairing();
    bool pairingActive() const { return pairing_; }
    uint32_t passkey() const { return passkey_; }
    const char* pairingName() const { return advertisingName_; }
    uint32_t pairingRemainingS() const;
    void poll(); // Closes the window on timeout; call from the main loop.

    size_t deviceCount() const { return resolver_.count(); }
    bool deviceAt(size_t index, antistalker::TrustIdentity& out) const;
    bool removeDevice(const char* identityText); // "54:EB:E9:77:8A:EA"
    bool renameDevice(const char* identityText, const char* name);

  private:
    // NimBLEServerCallbacks
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override;
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override;
    uint32_t onPassKeyDisplay() override;
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override;

    bool persist();

    antistalker::TrustResolver resolver_;
    BleMonitor& monitor_;
    NimBLEServer* server_ = nullptr;
    bool pairing_ = false;
    uint32_t pairingExpiresMs_ = 0;
    uint32_t passkey_ = 0;
    char advertisingName_[20] = {0};
};
