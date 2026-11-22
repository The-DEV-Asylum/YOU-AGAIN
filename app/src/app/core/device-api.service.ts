/*
 * ===========================================================================
 *  AntiStalker app :: device-api.service.ts
 *  HTTP client for the firmware API, plus paranoid response validation.
 * ===========================================================================
 *
 *  Why CapacitorHttp and not HttpClient/fetch? The firmware sends no CORS
 *  headers (on purpose, see the firmware docs). In a WebView that would
 *  block every call. CapacitorHttp goes through the native HTTP stack, so
 *  CORS does not apply, and the "X-AntiStalker: 1" action header still
 *  goes through.
 *
 *  Every field of every response is type-checked. If the board (or
 *  something pretending to be the board) sends junk, we throw a readable
 *  DeviceApiError instead of rendering "undefined dBm".
 * ===========================================================================
 */

import { Injectable } from '@angular/core';
import { Capacitor, CapacitorHttp, HttpResponse } from '@capacitor/core';

import {
  BatteryStatus,
  Candidate,
  DeviceStatus,
  HomePositionFix,
  HomeWifiConfig,
  HomeWifiStatus,
  PhoneLocation,
  PhoneLocationFix,
  TrustDevice,
  TrustState,
} from './antistalker.models';

export class DeviceApiError extends Error {
  constructor(message: string, readonly status?: number) {
    super(message);
    this.name = 'DeviceApiError';
  }
}

/**
 * Turns whatever the user typed ("192.168.4.1", "http://foo.local/") into a
 * clean base URL. Refuses other schemes, embedded credentials, queries and
 * fragments, because "ftp://admin:admin@..." is not a device address.
 */
export function normalizeEndpoint(value: string): string {
  const trimmed = value.trim();
  if (!trimmed) {
    throw new DeviceApiError('Enter the device address.');
  }

  if (/^[a-z][a-z\d+.-]*:\/\//i.test(trimmed) && !/^https?:\/\//i.test(trimmed)) {
    throw new DeviceApiError('Only HTTP or HTTPS device addresses are supported.');
  }

  let parsed: URL;
  try {
    parsed = new URL(/^https?:\/\//i.test(trimmed) ? trimmed : `http://${trimmed}`);
  } catch {
    throw new DeviceApiError('Enter a valid HTTP address, such as http://192.168.4.1.');
  }

  if (!['http:', 'https:'].includes(parsed.protocol) || parsed.username || parsed.password) {
    throw new DeviceApiError('Only HTTP or HTTPS device addresses without credentials are supported.');
  }
  if (parsed.search || parsed.hash) {
    throw new DeviceApiError('Remove query parameters or fragments from the device address.');
  }

  return parsed.toString().replace(/\/$/, '');
}

function record(value: unknown, label: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new DeviceApiError(`The device returned an invalid ${label} response.`);
  }
  return value as Record<string, unknown>;
}

function text(value: unknown, field: string): string {
  if (typeof value !== 'string') {
    throw new DeviceApiError(`The device response is missing ${field}.`);
  }
  return value;
}

function number(value: unknown, field: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new DeviceApiError(`The device response has an invalid ${field}.`);
  }
  return value;
}

function boolean(value: unknown, field: string): boolean {
  if (typeof value !== 'boolean') {
    throw new DeviceApiError(`The device response has an invalid ${field}.`);
  }
  return value;
}

function candidate(value: unknown): Candidate {
  const item = record(value, 'candidate');
  const state = text(item['state'], 'candidate state');
  const owner = text(item['owner'], 'owner state');
  if (!['NEARBY', 'PERSISTENT', 'FOLLOWING'].includes(state)) {
    throw new DeviceApiError(`The device returned an unknown candidate state: ${state}.`);
  }
  if (!['near', 'separated', 'unknown'].includes(owner)) {
    throw new DeviceApiError(`The device returned an unknown owner state: ${owner}.`);
  }

  const normalized: Candidate = {
    id: text(item['id'], 'candidate ID').toUpperCase(),
    type: text(item['type'], 'candidate type'),
    state: state as Candidate['state'],
    mac: text(item['mac'], 'MAC address'),
    addressType: number(item['addressType'], 'address type'),
    rssi: number(item['rssi'], 'RSSI'),
    peak: number(item['peak'], 'peak RSSI'),
    sightings: number(item['sightings'], 'sightings'),
    minutes: number(item['minutes'], 'distinct minutes'),
    locations: number(item['locations'], 'locations'),
    owner: owner as Candidate['owner'],
    lastSeen: number(item['lastSeen'], 'last-seen time'),
    allowed: boolean(item['allowed'], 'allow state'),
  };

  if (item['network'] !== undefined) {
    normalized.network = number(item['network'], 'DULT network');
  }
  // Firmware 0.5.0+ fields (trust, distance). Optional, so older boards still work.
  normalized.trusted = optionalBoolean(item['trusted']);
  normalized.trustedName = optionalText(item['trustedName']);
  normalized.distanceM = optionalNumber(item['distanceM']);
  normalized.distanceBand = optionalText(item['distanceBand']);
  return normalized;
}

function candidates(value: unknown, label: string): Candidate[] {
  if (!Array.isArray(value)) {
    throw new DeviceApiError(`The device response is missing ${label}.`);
  }
  return value.map(candidate);
}

function homeWifi(value: unknown): HomeWifiStatus {
  const item = record(value, 'home Wi-Fi status');
  return {
    enabled: boolean(item['enabled'], 'home Wi-Fi enabled state'),
    configured: boolean(item['configured'], 'home Wi-Fi configured state'),
    connected: boolean(item['connected'], 'home Wi-Fi connection state'),
    ssid: text(item['ssid'], 'home Wi-Fi SSID'),
    ip: text(item['ip'], 'home Wi-Fi IP address'),
  };
}

function optionalText(value: unknown): string | undefined {
  return typeof value === 'string' ? value : undefined;
}

function optionalNumber(value: unknown): number | undefined {
  return typeof value === 'number' && Number.isFinite(value) ? value : undefined;
}

function optionalBoolean(value: unknown): boolean | undefined {
  return typeof value === 'boolean' ? value : undefined;
}

function optionalString(value: unknown): string | undefined {
  return typeof value === 'string' ? value : undefined;
}

function optionalBattery(value: unknown): BatteryStatus | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  const item = record(value, 'battery status');
  return {
    available: boolean(item['available'], 'battery availability'),
    millivolts: item['millivolts'] == null ? null : number(item['millivolts'], 'battery millivolts'),
    percent: item['percent'] == null ? null : number(item['percent'], 'battery percent'),
  };
}

function optionalPhoneLocation(value: unknown): PhoneLocation | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  const item = record(value, 'phone location');
  return {
    valid: boolean(item['valid'], 'phone location validity'),
    latitude: number(item['latitude'], 'phone latitude'),
    longitude: number(item['longitude'], 'phone longitude'),
    accuracyM: number(item['accuracyM'], 'phone location accuracy'),
    ageS: number(item['ageS'], 'phone location age'),
    autoEpochDistanceM: number(item['autoEpochDistanceM'], 'auto-epoch distance'),
  };
}

function optionalTrust(value: unknown): TrustState | undefined {
  if (value === undefined || value === null) {
    return undefined;
  }
  const item = record(value, 'trusted-device state');
  const devicesRaw = item['devices'];
  if (!Array.isArray(devicesRaw)) {
    throw new DeviceApiError('The device response is missing the trusted-device list.');
  }
  const devices: TrustDevice[] = devicesRaw.map((entry) => {
    const device = record(entry, 'trusted device');
    return {
      name: text(device['name'], 'trusted device name'),
      identity: text(device['identity'], 'trusted device identity address'),
      addressType: number(device['addressType'], 'trusted device address type'),
      resolved: number(device['resolved'], 'trusted device resolution count'),
    };
  });
  return {
    pairing: boolean(item['pairing'], 'pairing state'),
    name: optionalText(item['name']),
    passkey: optionalText(item['passkey']),
    expiresS: optionalNumber(item['expiresS']),
    devices,
  };
}

function optionalHomePosition(value: unknown): DeviceStatus['home'] {
  if (value === undefined || value === null) {
    return undefined;
  }
  const item = record(value, 'home position');
  return {
    valid: boolean(item['valid'], 'home position validity'),
    latitude: item['latitude'] == null ? null : number(item['latitude'], 'home latitude'),
    longitude: item['longitude'] == null ? null : number(item['longitude'], 'home longitude'),
    accuracyM: item['accuracyM'] == null ? null : number(item['accuracyM'], 'home accuracy'),
    ageS: item['ageS'] == null ? null : number(item['ageS'], 'home position age'),
    source: typeof item['source'] === 'string' ? item['source'] : '',
    phoneDistanceM: item['phoneDistanceM'] == null
      ? null
      : number(item['phoneDistanceM'], 'phone distance from home'),
  };
}

export function normalizeStatus(value: unknown): DeviceStatus {
  const item = record(value, 'status');
  const normalized: DeviceStatus = {
    version: text(item['version'], 'firmware version'),
    ssid: text(item['ssid'], 'network name'),
    uptime: number(item['uptime'], 'uptime'),
    scanning: boolean(item['scanning'], 'scanner state'),
    received: number(item['received'], 'packet count'),
    dropped: number(item['dropped'], 'dropped-packet count'),
    trackerPackets: number(item['trackerPackets'], 'tracker-packet count'),
    devicePackets: number(item['devicePackets'], 'device-packet count'),
    devicesEnabled: boolean(item['devicesEnabled'], 'device-monitoring state'),
    locationEpoch: number(item['locationEpoch'], 'location epoch'),
    alerts: number(item['alerts'], 'alert count'),
    wifiClients: number(item['wifiClients'], 'Wi-Fi client count'),
    freeHeap: number(item['freeHeap'], 'free heap'),
    homeWifi: homeWifi(item['homeWifi']),
    trackers: candidates(item['trackers'], 'tracking tags'),
    devices: candidates(item['devices'], 'nearby devices'),
  };

  // Health and discovery fields are optional: an old firmware that does not
  // send them should give "not available", not a crash.
  normalized.mdns = optionalText(item['mdns']);
  normalized.chipModel = optionalText(item['chipModel']);
  normalized.totalHeap = optionalNumber(item['totalHeap']);
  normalized.freePsram = optionalNumber(item['freePsram']);
  normalized.totalPsram = optionalNumber(item['totalPsram']);
  normalized.sketchSize = optionalNumber(item['sketchSize']);
  normalized.flashFree = optionalNumber(item['flashFree']);
  normalized.maxCandidates = optionalNumber(item['maxCandidates']);
  normalized.temperatureC = item['temperatureC'] == null ? null : optionalNumber(item['temperatureC']);
  normalized.battery = optionalBattery(item['battery']);
  normalized.phoneLocation = optionalPhoneLocation(item['phoneLocation']);
  normalized.trust = optionalTrust(item['trust']);
  normalized.home = optionalHomePosition(item['home']);
  return normalized;
}

@Injectable({ providedIn: 'root' })
export class DeviceApiService {
  async getStatus(endpoint: string, options?: { timeoutMs?: number }): Promise<DeviceStatus> {
    const data = await this.request({
      method: 'GET',
      url: `${endpoint}/api/status`,
      headers: { Accept: 'application/json' },
      timeoutMs: options?.timeoutMs,
    });
    return normalizeStatus(this.responseData(data));
  }

  async markLocationMoved(endpoint: string): Promise<void> {
    await this.request({
      method: 'POST',
      url: `${endpoint}/api/moved`,
      headers: { 'X-AntiStalker': '1', Accept: 'application/json' },
    });
  }

  async sendLocation(endpoint: string, fix: PhoneLocationFix): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/location`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: fix,
      })),
      'phone location',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device returned an invalid phone location response.';
      throw new DeviceApiError(message);
    }
  }

  async setAllowed(endpoint: string, id: string, allowed: boolean): Promise<void> {
    await this.request({
      method: 'POST',
      url: `${endpoint}/api/allow`,
      params: { id, allowed: allowed ? '1' : '0' },
      headers: { 'X-AntiStalker': '1', Accept: 'application/json' },
    });
  }

  async setHomeWifi(endpoint: string, config: HomeWifiConfig): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/wifi`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: config,
      })),
      'home Wi-Fi configuration',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device returned an invalid home Wi-Fi configuration response.';
      throw new DeviceApiError(message);
    }
  }

  /** Opens the pairing window; the passkey arrives via the next status poll. */
  async startTrustPairing(endpoint: string, seconds = 300): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/trust/pair`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: { seconds },
      })),
      'trust pairing',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device could not open the pairing window.';
      throw new DeviceApiError(message);
    }
  }

  async cancelTrustPairing(endpoint: string): Promise<void> {
    await this.request({
      method: 'POST',
      url: `${endpoint}/api/trust/cancel`,
      headers: { 'X-AntiStalker': '1', Accept: 'application/json' },
    });
  }

  async renameTrustDevice(endpoint: string, identity: string, name: string): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/trust/name`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: { address: identity, name },
      })),
      'trusted-device rename',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device could not rename the trusted device.';
      throw new DeviceApiError(message);
    }
  }

  async removeTrustDevice(endpoint: string, identity: string): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/trust/remove`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: { address: identity },
      })),
      'trusted-device removal',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device could not remove the trusted device.';
      throw new DeviceApiError(message);
    }
  }

  async setHomePosition(endpoint: string, fix: HomePositionFix): Promise<void> {
    const data = record(
      this.responseData(await this.request({
        method: 'POST',
        url: `${endpoint}/api/home`,
        headers: {
          'X-AntiStalker': '1',
          Accept: 'application/json',
          'Content-Type': 'application/json',
        },
        data: fix,
      })),
      'home position',
    );
    if (data['ok'] !== true) {
      const message = typeof data['error'] === 'string'
        ? data['error']
        : 'The device could not save the home position.';
      throw new DeviceApiError(message);
    }
  }

  async removeHomePosition(endpoint: string): Promise<void> {
    await this.request({
      method: 'POST',
      url: `${endpoint}/api/home/remove`,
      headers: { 'X-AntiStalker': '1', Accept: 'application/json' },
    });
  }

  private async request(options: {
    method: 'GET' | 'POST';
    url: string;
    headers: Record<string, string>;
    params?: Record<string, string>;
    data?: object;
    timeoutMs?: number;
  }): Promise<unknown> {
    const connectTimeout = options.timeoutMs ?? 4500;
    const readTimeout = options.timeoutMs ? Math.max(options.timeoutMs, 2000) : 6000;
    let response: HttpResponse;
    try {
      response = await this.withWebTimeout(
        CapacitorHttp.request({
          ...options,
          connectTimeout,
          readTimeout,
          responseType: 'json',
          webFetchExtra: { cache: 'no-store' },
        }),
        connectTimeout + readTimeout + 1000,
      );
    } catch (error) {
      const detail = error instanceof Error ? error.message : '';
      throw new DeviceApiError(detail || 'The device could not be reached.');
    }

    if (response.status < 200 || response.status >= 300) {
      const data = record(this.responseData(response.data), 'error');
      const message = typeof data['error'] === 'string' ? data['error'] : `HTTP ${response.status}`;
      throw new DeviceApiError(message, response.status);
    }
    return response.data as unknown;
  }

  /**
   * Native platforms honour the CapacitorHttp timeouts. The web fallback
   * uses fetch(), which happily waits forever. So on web only, we race a
   * timer, and an unreachable address fails in seconds instead of never.
   */
  private async withWebTimeout<T>(request: Promise<T>, timeoutMs: number): Promise<T> {
    if (Capacitor.getPlatform() !== 'web') {
      return request;
    }
    let timer: ReturnType<typeof setTimeout> | undefined;
    try {
      return await Promise.race([
        request,
        new Promise<never>((_, reject) => {
          timer = setTimeout(
            () => reject(new Error('The device could not be reached.')),
            timeoutMs,
          );
        }),
      ]);
    } finally {
      clearTimeout(timer);
    }
  }

  private responseData(data: unknown): unknown {
    if (typeof data !== 'string') {
      return data;
    }
    try {
      return JSON.parse(data) as unknown;
    } catch {
      throw new DeviceApiError('The device returned unreadable JSON.');
    }
  }
}
