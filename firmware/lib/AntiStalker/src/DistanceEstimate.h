#pragma once

namespace antistalker {

/*
 * ===========================================================================
 *  AntiStalker :: DistanceEstimate.h
 *  RSSI -> meters. Or: how to lie with physics, but honestly.
 * ===========================================================================
 *
 *  Log-distance path-loss model:
 *
 *      d = 10 ^ ((RSSI_at_1m - RSSI) / (10 * n))
 *
 *  Real world: walls, your body (you are a big bag of salty water, 2.4 GHz
 *  loves that), the tag being in a pocket, antenna orientation. The true
 *  distance is often 2x off in either direction. So every number ships with
 *  a band ("about 1-4 m"), and the UI never shows one without the other.
 * ===========================================================================
 */

// RSSI at 1 meter. -59 dBm is the iBeacon convention; real devices land
// anywhere around -49..-65 dBm. It is a guess, a well-documented guess.
constexpr int kReferenceRssi1m = -59;
// Path-loss exponent. 2.0 = free space (lab, vacuum, dreams), 4+ = concrete
// jungle. 2.5 = "indoors, some stuff in the way".
constexpr float kPathLossExponent = 2.5f;

// Log-distance path-loss estimate in meters, clamped to [0.5, 100].
int estimateDistanceM(int rssi);

// Human-readable band, so the number never pretends to be precise.
const char* distanceBand(int rssi);

} // namespace antistalker
