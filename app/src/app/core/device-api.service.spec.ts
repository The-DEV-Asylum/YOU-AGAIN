import { TestBed } from '@angular/core/testing';
import { Capacitor, CapacitorHttp, HttpResponse } from '@capacitor/core';
import { afterEach, describe, expect, it, vi } from 'vitest';

vi.mock('@capacitor/core', () => ({
  Capacitor: { getPlatform: vi.fn().mockReturnValue('native') },
  CapacitorHttp: { request: vi.fn() },
}));

import {
  DeviceApiError,
  DeviceApiService,
  normalizeEndpoint,
  normalizeStatus,
} from './device-api.service';

const candidate = {
  id: 'a1b2c3d4',
  type: 'Apple Find My',
  state: 'PERSISTENT',
  mac: 'AA:BB:CC:DD:EE:FF',
  addressType: 1,
  rssi: -67,
  peak: -51,
  sightings: 12,
  minutes: 5,
  locations: 1,
  owner: 'unknown',
  lastSeen: 2,
  allowed: false,
};

const status = {
  version: '0.4.0',
  ssid: 'AntiStalker-C398',
  uptime: 230,
  scanning: true,
  received: 800,
  dropped: 0,
  trackerPackets: 40,
  devicePackets: 160,
  devicesEnabled: true,
  locationEpoch: 2,
  alerts: 1,
  wifiClients: 1,
  freeHeap: 250000,
  mdns: 'antistalker-c398.local',
  chipModel: 'ESP32-S3',
  totalHeap: 320000,
  freePsram: 7800000,
  totalPsram: 8388608,
  sketchSize: 1400000,
  flashFree: 12000000,
  maxCandidates: 64,
  temperatureC: 42.5,
  battery: {
    available: true,
    millivolts: 3960,
    percent: 66,
  },
  homeWifi: {
    enabled: true,
    configured: true,
    connected: true,
    ssid: 'HomeNetwork',
    ip: '192.168.1.42',
  },
  phoneLocation: {
    valid: true,
    latitude: 52.5,
    longitude: 13.4,
    accuracyM: 12,
    ageS: 9,
    autoEpochDistanceM: 150,
  },
  trackers: [candidate],
  devices: [],
};

function response(data: unknown, httpStatus = 200): HttpResponse {
  return { data, status: httpStatus, headers: {}, url: 'http://192.168.4.1' };
}

describe('normalizeEndpoint', () => {
  it('accepts the firmware address and adds HTTP when omitted', () => {
    expect(normalizeEndpoint('192.168.4.1')).toBe('http://192.168.4.1');
    expect(normalizeEndpoint('http://192.168.4.1/')).toBe('http://192.168.4.1');
  });

  it('rejects unsupported or credentialed addresses', () => {
    expect(() => normalizeEndpoint('ftp://192.168.4.1')).toThrow(DeviceApiError);
    expect(() => normalizeEndpoint('http://user:pass@192.168.4.1')).toThrow(DeviceApiError);
    expect(() => normalizeEndpoint('http://192.168.4.1/?x=1')).toThrow(DeviceApiError);
    expect(() => normalizeEndpoint('http://192.168.4.1/#/status')).toThrow(DeviceApiError);
  });
});

describe('normalizeStatus', () => {
  it('normalizes the complete firmware payload', () => {
    const result = normalizeStatus(status);
    expect(result.ssid).toBe('AntiStalker-C398');
    expect(result.trackers[0].id).toBe('A1B2C3D4');
    expect(result.trackers[0].state).toBe('PERSISTENT');
    expect(result.homeWifi).toEqual({
      enabled: true,
      configured: true,
      connected: true,
      ssid: 'HomeNetwork',
      ip: '192.168.1.42',
    });
    expect(result.homeWifi).not.toHaveProperty('password');
    expect(result.mdns).toBe('antistalker-c398.local');
    expect(result.temperatureC).toBe(42.5);
    expect(result.battery).toEqual({ available: true, millivolts: 3960, percent: 66 });
    expect(result.phoneLocation).toEqual({
      valid: true,
      latitude: 52.5,
      longitude: 13.4,
      accuracyM: 12,
      ageS: 9,
      autoEpochDistanceM: 150,
    });
    expect(result.freePsram).toBe(7800000);
    expect(result.flashFree).toBe(12000000);
    expect(result.maxCandidates).toBe(64);
  });

  it('treats device health fields as optional for older firmware', () => {
    const legacy = { ...status } as Record<string, unknown>;
    for (const key of [
      'mdns', 'chipModel', 'totalHeap', 'freePsram', 'totalPsram', 'sketchSize',
      'flashFree', 'maxCandidates', 'temperatureC', 'battery', 'phoneLocation',
    ]) {
      delete legacy[key];
    }
    const result = normalizeStatus(legacy);
    expect(result.version).toBe('0.4.0');
    expect(result.mdns).toBeUndefined();
    expect(result.temperatureC).toBeNull();
    expect(result.battery).toBeUndefined();
    expect(result.phoneLocation).toBeUndefined();
    expect(result.flashFree).toBeUndefined();
  });

  it('rejects malformed responses rather than rendering unsafe assumptions', () => {
    expect(() => normalizeStatus({})).toThrow(DeviceApiError);
    expect(() => normalizeStatus({ ...status, homeWifi: { enabled: true } })).toThrow(DeviceApiError);
    expect(() => normalizeStatus({ ...status, trackers: [{ ...candidate, state: 'UNKNOWN' }] })).toThrow(
      DeviceApiError,
    );
    expect(() => normalizeStatus({ ...status, battery: { available: 'yes' } })).toThrow(DeviceApiError);
    expect(() => normalizeStatus({ ...status, trust: { pairing: 'yes' } })).toThrow(DeviceApiError);
    expect(() => normalizeStatus({ ...status, home: { valid: 'true' } })).toThrow(DeviceApiError);
  });

  it('normalizes firmware 0.5.0 trust and home state', () => {
    const result = normalizeStatus({
      ...status,
      version: '0.5.0',
      trackers: [{
        ...candidate,
        trusted: true,
        trustedName: 'My phone',
        distanceM: 3.2,
        distanceBand: 'about 2-5 m',
      }],
      trust: {
        pairing: true,
        name: 'AntiStalker-C398',
        passkey: '056476',
        expiresS: 240,
        devices: [
          { name: 'android', identity: 'BC:A5:8B:1F:78:FC', addressType: 0, resolved: 3 },
        ],
      },
      home: {
        valid: true,
        latitude: 48.858,
        longitude: 2.294,
        accuracyM: 15,
        ageS: 30,
        source: 'phone',
        phoneDistanceM: 120,
      },
    });

    expect(result.trackers[0].trusted).toBe(true);
    expect(result.trackers[0].trustedName).toBe('My phone');
    expect(result.trackers[0].distanceM).toBe(3.2);
    expect(result.trackers[0].distanceBand).toBe('about 2-5 m');
    expect(result.trust).toEqual({
      pairing: true,
      name: 'AntiStalker-C398',
      passkey: '056476',
      expiresS: 240,
      devices: [{ name: 'android', identity: 'BC:A5:8B:1F:78:FC', addressType: 0, resolved: 3 }],
    });
    expect(result.home).toEqual({
      valid: true,
      latitude: 48.858,
      longitude: 2.294,
      accuracyM: 15,
      ageS: 30,
      source: 'phone',
      phoneDistanceM: 120,
    });
  });

  it('keeps trust and home optional for older firmware', () => {
    const result = normalizeStatus(status);
    expect(result.trust).toBeUndefined();
    expect(result.home).toBeUndefined();
    expect(result.trackers[0].trusted).toBeUndefined();
    expect(result.trackers[0].distanceM).toBeUndefined();
  });

  it('accepts a null-valued home position object', () => {
    const result = normalizeStatus({
      ...status,
      home: {
        valid: false,
        latitude: null,
        longitude: null,
        accuracyM: null,
        ageS: null,
        source: '',
        phoneDistanceM: null,
      },
    });
    expect(result.home).toEqual({
      valid: false,
      latitude: null,
      longitude: null,
      accuracyM: null,
      ageS: null,
      source: '',
      phoneDistanceM: null,
    });
  });
});

describe('DeviceApiService', () => {
  afterEach(() => vi.clearAllMocks());

  it('uses Capacitor native HTTP for status', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response(status));
    const service = TestBed.inject(DeviceApiService);

    await expect(service.getStatus('http://192.168.4.1')).resolves.toMatchObject({ version: '0.4.0' });
    expect(request).toHaveBeenCalledWith(expect.objectContaining({
      method: 'GET',
      url: 'http://192.168.4.1/api/status',
      responseType: 'json',
    }));
  });

  it('sends the firmware action header and encoded candidate parameters', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.setAllowed('http://192.168.4.1', 'A1B2C3D4', true);
    await service.markLocationMoved('http://192.168.4.1');

    expect(request).toHaveBeenNthCalledWith(1, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/allow',
      params: { id: 'A1B2C3D4', allowed: '1' },
      headers: expect.objectContaining({ 'X-AntiStalker': '1' }),
    }));
    expect(request).toHaveBeenNthCalledWith(2, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/moved',
      headers: expect.objectContaining({ 'X-AntiStalker': '1' }),
    }));
  });

  it('shares a phone GPS fix as native HTTP JSON data', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.sendLocation('http://192.168.4.1', {
      latitude: 52.5,
      longitude: 13.4,
      accuracy: 12,
    });

    expect(request).toHaveBeenCalledWith(expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/location',
      data: { latitude: 52.5, longitude: 13.4, accuracy: 12 },
      headers: expect.objectContaining({
        'X-AntiStalker': '1',
        'Content-Type': 'application/json',
      }),
    }));
  });

  it('uses a short timeout when probing for discovery', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response(status));
    const service = TestBed.inject(DeviceApiService);

    await service.getStatus('http://192.168.4.1', { timeoutMs: 1600 });

    expect(request).toHaveBeenCalledWith(expect.objectContaining({
      connectTimeout: 1600,
      readTimeout: 2000,
    }));
  });

  it('sends a toggle-only home Wi-Fi request without credentials', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.setHomeWifi('http://192.168.4.1', { enabled: false });

    expect(request).toHaveBeenCalledWith(expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/wifi',
      data: { enabled: false },
      headers: expect.objectContaining({
        'X-AntiStalker': '1',
        'Content-Type': 'application/json',
      }),
    }));
  });

  it('sends home Wi-Fi credentials as native HTTP JSON data', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.setHomeWifi('http://192.168.4.1', {
      enabled: true,
      ssid: 'HomeNetwork',
      password: 'secret',
    });

    expect(request).toHaveBeenCalledWith(expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/wifi',
      data: { enabled: true, ssid: 'HomeNetwork', password: 'secret' },
      headers: expect.objectContaining({
        'X-AntiStalker': '1',
        'Content-Type': 'application/json',
      }),
    }));
  });

  it('opens and cancels the trusted-device pairing window', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(
      response({ ok: true, passkey: '056476', name: 'AntiStalker-C398' }),
    );
    const service = TestBed.inject(DeviceApiService);

    await service.startTrustPairing('http://192.168.4.1', 120);
    await service.cancelTrustPairing('http://192.168.4.1');

    expect(request).toHaveBeenNthCalledWith(1, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/trust/pair',
      data: { seconds: 120 },
      headers: expect.objectContaining({
        'X-AntiStalker': '1',
        'Content-Type': 'application/json',
      }),
    }));
    expect(request).toHaveBeenNthCalledWith(2, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/trust/cancel',
      headers: expect.objectContaining({ 'X-AntiStalker': '1' }),
    }));
  });

  it('renames and removes trusted devices', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.renameTrustDevice('http://192.168.4.1', 'BC:A5:8B:1F:78:FC', 'My phone');
    await service.removeTrustDevice('http://192.168.4.1', 'BC:A5:8B:1F:78:FC');

    expect(request).toHaveBeenNthCalledWith(1, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/trust/name',
      data: { address: 'BC:A5:8B:1F:78:FC', name: 'My phone' },
    }));
    expect(request).toHaveBeenNthCalledWith(2, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/trust/remove',
      data: { address: 'BC:A5:8B:1F:78:FC' },
    }));
  });

  it('saves and clears the home position', async () => {
    const request = vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ ok: true }));
    const service = TestBed.inject(DeviceApiService);

    await service.setHomePosition('http://192.168.4.1', { latitude: 48.85, longitude: 2.29, source: 'map' });
    await service.removeHomePosition('http://192.168.4.1');

    expect(request).toHaveBeenNthCalledWith(1, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/home',
      data: { latitude: 48.85, longitude: 2.29, source: 'map' },
    }));
    expect(request).toHaveBeenNthCalledWith(2, expect.objectContaining({
      method: 'POST',
      url: 'http://192.168.4.1/api/home/remove',
    }));
  });

  it('surfaces firmware errors', async () => {
    vi.mocked(CapacitorHttp.request).mockResolvedValue(response({ error: 'candidate not found' }, 404));
    const service = TestBed.inject(DeviceApiService);
    await expect(service.setAllowed('http://192.168.4.1', 'FFFFFFFF', true)).rejects.toThrow(
      'candidate not found',
    );
  });
});
