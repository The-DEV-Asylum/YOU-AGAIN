/*
 * ===========================================================================
 *  AntiStalker :: BleMonitor.h
 *  The ears. Listens to every BLE advert in the room and never talks back.
 * ===========================================================================
 *
 *  Pipeline:
 *
 *      NimBLE host task (radio context)
 *              |
 *              |  onResult(): copy address/RSSI/payload, NO parsing here
 *              V
 *      FreeRTOS queue (24 slots, drops are counted, never blocked on)
 *              |
 *              |  poll() from loop(): max 16 events per pass
 *              V
 *      trust resolution (IRK) -> tracker classifier -> personal classifier
 *              |
 *              V
 *      trackerEngine  or  deviceEngine
 *
 *  Why the queue? The NimBLE callback runs in the BLE host task. Doing heavy
 *  work there (parsing, hashing, Serial.printf) stalls the stack and you
 *  start losing adverts. So the callback only memcpy's and leaves.
 *
 *  Passive means passive: active scanning is OFF, so we never send a scan
 *  request. Nobody nearby can tell this board is listening.
 * ===========================================================================
 */

#pragma once

#include "DetectionEngine.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace antistalker {
class TrustResolver;
} // namespace antistalker

class BleMonitor : private NimBLEScanCallbacks {
  public:
    bool begin();
    void poll(antistalker::DetectionEngine& trackerEngine, antistalker::DetectionEngine& deviceEngine);

    void setVerbose(bool enabled) { verbose_ = enabled; }
    bool verbose() const { return verbose_; }
    void setPersonalDevicesEnabled(bool enabled) { personalDevicesEnabled_ = enabled; }
    bool personalDevicesEnabled() const { return personalDevicesEnabled_; }
    // Hands over the IRK table so bonded devices get recognised passively,
    // whatever rotating address they are wearing today.
    void setTrustResolver(antistalker::TrustResolver& resolver) { trust_ = &resolver; }
    // Paused while a pairing window is open (NimBLE cannot rebuild its GATT
    // table while a scan is running, see TrustManager::startPairing()).
    void setScanningEnabled(bool enabled);
    void setLocateId(uint32_t id);
    void stopLocating();

    uint32_t receivedAdvertisements() const { return receivedAdvertisements_; }
    uint32_t matchedAdvertisements() const { return matchedAdvertisements_; }
    uint32_t personalAdvertisements() const { return personalAdvertisements_; }
    uint32_t droppedAdvertisements() const { return droppedAdvertisements_; }
    bool scanning();

  private:
    // 255 = biggest BLE 5 extended advert we accept (CONFIG_BT_NIMBLE_MAX_EXT_ADV_DATA_LEN).
    static constexpr size_t kMaxPayloadBytes = 255;
    // 24 events x ~310 bytes. Big enough for a busy train, small enough to
    // not care. If 'dropped' climbs in stats, this is the knob.
    static constexpr size_t kQueueDepth = 24;

    // One advert, copied out of NimBLE so the radio task can move on.
    struct AdvertisementEvent {
        char address[18];
        char name[25];
        uint8_t addressType;
        int rssi;
        uint32_t nowMs;
        uint16_t payloadLength;
        bool trusted;
        uint8_t payload[kMaxPayloadBytes];
    };

    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override;
    void onScanEnd(const NimBLEScanResults& scanResults, int reason) override;
    void processEvent(const AdvertisementEvent& event, antistalker::DetectionEngine& trackerEngine,
                      antistalker::DetectionEngine& deviceEngine);
    void processUpdate(const AdvertisementEvent& event, const antistalker::TrackerMatch& match,
                       antistalker::DetectionEngine& engine, bool personalDevice);
    void printVerbose(const AdvertisementEvent& event, antistalker::TrackerFamily family) const;

    NimBLEScan* scan_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    // volatile = written from the NimBLE task, read from loop(). Counters
    // only; a torn read just means stats are off by one for a moment.
    volatile uint32_t receivedAdvertisements_ = 0;
    volatile uint32_t droppedAdvertisements_ = 0;
    uint32_t matchedAdvertisements_ = 0;
    uint32_t personalAdvertisements_ = 0;
    uint32_t lastLocatePrintMs_ = 0;
    uint32_t locateId_ = 0;
    bool locating_ = false;
    bool verbose_ = false;
    bool personalDevicesEnabled_ = true;
    bool scanningEnabled_ = true;
    antistalker::TrustResolver* trust_ = nullptr;
    volatile bool restartRequested_ = false;
};
