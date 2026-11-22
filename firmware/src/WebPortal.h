/*
 * ===========================================================================
 *  AntiStalker :: WebPortal.h
 *  Wi-Fi, mDNS, the embedded dashboard and the JSON API. The mouth.
 * ===========================================================================
 *
 *  Network modes (decided at runtime, never by the cloud, there is no cloud):
 *
 *      home Wi-Fi saved + enabled + reachable?
 *              |
 *        yes --+--> STA on home network, fallback AP OFF,
 *              |    reachable at http://<dhcp-ip> or antistalker-XXXX.local
 *              |
 *         no --+--> fallback AP "AntiStalker-XXXX" (WPA2), http://192.168.4.1
 *                   and retry home every 30 s if one is configured
 *
 *  API shape:
 *    GET  /              -> the dashboard (one HTML blob, no external assets)
 *    GET  /api/status    -> everything, as JSON
 *    POST /api/...       -> actions, and ALL of them need the header
 *                           "X-AntiStalker: 1"
 *
 *  That header is NOT authentication. It is a cross-origin speed bump: a
 *  random web page in your browser cannot send a custom header to this board
 *  without a CORS preflight, and the board answers no preflights. Anyone on
 *  the same network can still read /api/status. Keep the AP password private
 *  and only join trusted home networks. (Full story in docs/THREAT_MODEL.md.)
 * ===========================================================================
 */

#pragma once

#include "DetectionEngine.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

class BleMonitor;
class TrustManager;

// Latest GPS fix pushed by the companion app (opt-in). RAM only: reboot the
// board and it forgets where you have been. On purpose.
struct PhoneLocation {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float accuracyM = 0.0f; // 0 means the phone did not report an accuracy.
    uint32_t receivedMs = 0;
};

// Optional "home" position set by the owner from the app or dashboard.
// Persisted in NVS. Context only ("phone is ~40 m from home").
// It NEVER suppresses alerts. Tempting, but wrong: a tag hidden in your bag
// comes home with you. "You're home, relax" is exactly the wrong answer.
struct HomeLocation {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    float accuracyM = 0.0f;
    uint32_t setMs = 0;
    char source[12] = {0}; // "phone", "map" or "manual"
};

class WebPortal {
  public:
    WebPortal(antistalker::DetectionEngine& trackerEngine,
              antistalker::DetectionEngine& deviceEngine, BleMonitor& monitor,
              TrustManager& trust);

    bool begin();
    void stop();
    void handle();
    void printCredentials() const;

    bool running() const { return running_; }
    const char* ssid() const { return ssid_; }
    const char* password() const { return password_; }
    const char* hostname() const { return hostname_; }
    const PhoneLocation& phoneLocation() const { return phoneLocation_; }

  private:
    void configureRoutes();
    void sendIndex();
    void sendStatus();
    void handleMoved();
    void handleAllow();
    void handleWifi();
    void handleLocation();
    void handleTrustPair();
    void handleTrustCancel();
    void handleTrustRename();
    void handleTrustRemove();
    void handleHome();
    void handleHomeRemove();
    void sendJson(const String& json, int code = 200);
    bool actionAuthorized();
    bool setAllowedFromRequest();
    bool updateHomeWifiFromRequest();
    bool updatePhoneLocationFromRequest();
    bool updateHomeFromRequest();
    bool persistHome(const HomeLocation& next);
    void connectHome();
    bool startAccessPoint();
    void stopAccessPoint();
    void startMdns();
    void adoptLocationAnchor();
    void appendSystem(JsonDocument& doc);
    void appendPhoneLocation(JsonDocument& doc);
    void appendHome(JsonDocument& doc);
    void appendTrust(JsonDocument& doc);
    void appendCandidates(JsonArray array, const antistalker::DetectionEngine& engine,
                          uint32_t nowMs);

    antistalker::DetectionEngine& trackerEngine_;
    antistalker::DetectionEngine& deviceEngine_;
    BleMonitor& monitor_;
    TrustManager& trust_;
    WebServer server_;
    char ssid_[32] = {0};
    char password_[24] = {0};
    char hostname_[32] = {0};
    String homeSsid_;
    String homePassword_;
    PhoneLocation phoneLocation_;
    // Where the current location epoch "started". Move ~150 m away from it
    // (with a decent GPS fix) and a new epoch begins automatically.
    PhoneLocation epochAnchor_;
    HomeLocation home_;
    bool homeEnabled_ = false;
    bool homeReconnectPending_ = false;
    bool homeWasConnected_ = false;
    uint32_t homeReconnectAtMs_ = 0;
    uint32_t lastHomeAttemptMs_ = 0;
    uint32_t lastApAttemptMs_ = 0;
    bool apRunning_ = false;
    bool running_ = false;
    bool routesConfigured_ = false;
};
