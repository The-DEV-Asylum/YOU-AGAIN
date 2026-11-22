/*
 * ===========================================================================
 *  AntiStalker app :: antistalker.models.ts
 *  API types, defaults, and small pure helpers (filtering, labels, maths).
 * ===========================================================================
 *
 *  Pure TypeScript, no Angular, no Capacitor. Easy to unit test, hard to
 *  break by accident. Field names mirror the firmware JSON 1:1.
 * ===========================================================================
 */

export type CandidateKind = 'tracker' | 'device';
export type CandidateFilter = 'all' | 'attention' | 'allowed';
export type ConnectionState = 'idle' | 'connecting' | 'connected' | 'disconnected';

/** Candidates seen within this window are presented as "active now". */
export const ACTIVE_WINDOW_S = 120;

export interface Candidate {
  id: string;
  type: string;
  state: 'NEARBY' | 'PERSISTENT' | 'FOLLOWING';
  mac: string;
  addressType: number;
  rssi: number;
  peak: number;
  sightings: number;
  minutes: number;
  locations: number;
  owner: 'near' | 'separated' | 'unknown';
  lastSeen: number;
  allowed: boolean;
  // Firmware 0.5.0+. Optional, so the app still talks to older boards.
  trusted?: boolean;
  trustedName?: string;
  distanceM?: number;
  distanceBand?: string;
  network?: number;
}

export interface HomeWifiStatus {
  enabled: boolean;
  configured: boolean;
  connected: boolean;
  ssid: string;
  ip: string;
}

export type HomeWifiConfig =
  | { enabled: boolean }
  | { enabled: boolean; ssid: string; password: string };

export interface BatteryStatus {
  available: boolean;
  millivolts: number | null;
  percent: number | null;
}

export interface PhoneLocation {
  valid: boolean;
  latitude: number;
  longitude: number;
  accuracyM: number;
  ageS: number;
  autoEpochDistanceM: number;
}

/** Fix reported by the phone's own GPS to share with the device. */
export interface PhoneLocationFix {
  latitude: number;
  longitude: number;
  accuracy?: number;
}

/** A device bonded once over BLE whose rotating addresses are recognized. */
export interface TrustDevice {
  name: string;
  identity: string;
  addressType: number;
  resolved: number;
}

/** Live trusted-device state, including an open pairing window. */
export interface TrustState {
  pairing: boolean;
  name?: string;
  /** 6-digit code as a string, so "004217" does not become 4217. */
  passkey?: string;
  expiresS?: number;
  devices: TrustDevice[];
}

/** Home position set by the owner. Context only, NEVER an alert filter (the tag in your bag comes home too). */
export interface HomePosition {
  valid: boolean;
  latitude: number | null;
  longitude: number | null;
  accuracyM: number | null;
  ageS: number | null;
  source: string;
  phoneDistanceM: number | null;
}

/** Home coordinates to store on the device. */
export interface HomePositionFix {
  latitude: number;
  longitude: number;
  accuracy?: number;
  source: 'phone' | 'map' | 'manual';
}

export interface DeviceStatus {
  version: string;
  ssid: string;
  uptime: number;
  scanning: boolean;
  received: number;
  dropped: number;
  trackerPackets: number;
  devicePackets: number;
  devicesEnabled: boolean;
  locationEpoch: number;
  alerts: number;
  wifiClients: number;
  freeHeap: number;
  homeWifi: HomeWifiStatus;
  trackers: Candidate[];
  devices: Candidate[];
  // Health and discovery fields (firmware 0.4.0+). Optional, same reason.
  mdns?: string;
  chipModel?: string;
  totalHeap?: number;
  freePsram?: number;
  totalPsram?: number;
  sketchSize?: number;
  flashFree?: number;
  maxCandidates?: number;
  temperatureC?: number | null;
  battery?: BatteryStatus;
  phoneLocation?: PhoneLocation;
  // Firmware 0.5.0+ fields (optional for compatibility with older builds).
  trust?: TrustState;
  home?: HomePosition;
}

export interface AppSettings {
  endpoint: string;
  autoRefresh: boolean;
  refreshInterval: number;
  /** Last known home Wi-Fi IP and mDNS name of the paired device. */
  knownHomeIp?: string;
  mdnsHost?: string;
  /** Opt-in: share the phone's GPS fix so the device advances location epochs automatically. */
  locationSharing: boolean;
}

export const DEFAULT_SETTINGS: AppSettings = {
  endpoint: 'http://192.168.4.1',
  autoRefresh: true,
  refreshInterval: 3,
  locationSharing: false,
};

/** Number of candidates whose last advertisement arrived within the window. */
export function activeCount(candidates: Candidate[], maxAgeS = ACTIVE_WINDOW_S): number {
  return candidates.filter((candidate) => candidate.lastSeen <= maxAgeS).length;
}

export function distanceMeters(lat1: number, lon1: number, lat2: number, lon2: number): number {
  const toRadians = (degrees: number): number => (degrees * Math.PI) / 180;
  const dLat = toRadians(lat2 - lat1);
  const dLon = toRadians(lon2 - lon1);
  const a =
    Math.sin(dLat / 2) ** 2 +
    Math.cos(toRadians(lat1)) * Math.cos(toRadians(lat2)) * Math.sin(dLon / 2) ** 2;
  return 2 * 6371000 * Math.asin(Math.sqrt(a));
}

export function formatBytes(bytes: number): string {
  if (bytes <= 0 || !Number.isFinite(bytes)) {
    return '0';
  }
  const units = ['B', 'KiB', 'MiB', 'GiB'];
  const exponent = Math.min(Math.floor(Math.log2(bytes) / 10), units.length - 1);
  const value = bytes / 2 ** (10 * exponent);
  return `${exponent === 0 ? value : value.toFixed(1)} ${units[exponent]}`;
}

export function displayState(candidate: Candidate, kind: CandidateKind): string {
  return kind === 'device' && candidate.state === 'FOLLOWING'
    ? 'CO-PRESENT'
    : candidate.state;
}

export function isElevated(candidate: Candidate): boolean {
  return candidate.state === 'PERSISTENT' || candidate.state === 'FOLLOWING';
}

export function candidateStateClass(candidate: Candidate): string {
  if (candidate.allowed || candidate.trusted) {
    return 'allowed';
  }
  if (candidate.state === 'FOLLOWING') {
    return 'following';
  }
  if (candidate.state === 'PERSISTENT') {
    return 'persistent';
  }
  return 'nearby';
}

export function filterCandidates(
  candidates: Candidate[],
  kind: CandidateKind,
  query: string,
  filter: CandidateFilter,
): Candidate[] {
  const normalizedQuery = query.trim().toLowerCase();

  return candidates
    .filter((candidate) => {
      if (filter === 'attention' && (!isElevated(candidate) || candidate.allowed)) {
        return false;
      }
      if (filter === 'allowed' && !candidate.allowed) {
        return false;
      }
      if (!normalizedQuery) {
        return true;
      }

      return [
        candidate.id,
        candidate.type,
        displayState(candidate, kind),
        candidate.state,
        candidate.mac,
        candidate.addressType,
        candidate.rssi,
        candidate.peak,
        candidate.sightings,
        candidate.minutes,
        candidate.locations,
        candidate.lastSeen,
        candidate.owner,
        candidate.allowed ? 'allowed' : 'active',
        candidate.trusted ? 'trusted' : '',
        candidate.trustedName ?? '',
      ]
        .join(' ')
        .toLowerCase()
        .includes(normalizedQuery);
    })
    .sort((a, b) => {
      if (a.allowed !== b.allowed) {
        return a.allowed ? 1 : -1;
      }
      const severity = { NEARBY: 0, PERSISTENT: 1, FOLLOWING: 2 };
      return severity[b.state] - severity[a.state] || a.lastSeen - b.lastSeen;
    });
}

export function formatLastSeen(seconds: number): string {
  if (seconds < 5) {
    return 'Now';
  }
  if (seconds < 60) {
    return `${seconds}s ago`;
  }
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) {
    return `${minutes}m ago`;
  }
  return `${Math.floor(minutes / 60)}h ago`;
}

export function formatUptime(seconds: number): string {
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) {
    return `${days}d ${hours}h`;
  }
  if (hours > 0) {
    return `${hours}h ${minutes}m`;
  }
  return `${minutes}m ${seconds % 60}s`;
}
