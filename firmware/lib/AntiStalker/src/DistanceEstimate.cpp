#include "DistanceEstimate.h"

#include <math.h>

namespace antistalker {

int estimateDistanceM(int rssi) {
    // d = 10^((txPower1m - RSSI) / (10 * n))
    const float exponent = (kReferenceRssi1m - static_cast<float>(rssi)) /
                          (10.0f * kPathLossExponent);
    float meters = powf(10.0f, exponent);
    if (meters < 0.5f) {
        meters = 0.5f;
    }
    if (meters > 100.0f) {
        meters = 100.0f;
    }
    return static_cast<int>(meters + 0.5f);
}

const char* distanceBand(int rssi) {
    if (rssi >= -60) {
        return "under 1 m";
    }
    if (rssi >= -75) {
        return "about 1-4 m";
    }
    if (rssi >= -85) {
        return "about 4-10 m";
    }
    if (rssi >= -95) {
        return "about 10-30 m";
    }
    return "over 30 m";
}

} // namespace antistalker
