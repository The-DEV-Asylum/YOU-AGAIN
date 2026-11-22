import { TestBed } from '@angular/core/testing';
import { Geolocation } from '@capacitor/geolocation';
import { vi } from 'vitest';

import { DeviceStatus } from './antistalker.models';
import { DeviceApiService } from './device-api.service';
import { MonitorService } from './monitor.service';

vi.mock('@capacitor/preferences', () => ({
  Preferences: {
    get: vi.fn().mockResolvedValue({ value: null }),
    set: vi.fn().mockResolvedValue(undefined),
  },
}));

vi.mock('@capacitor/geolocation', () => ({
  Geolocation: {
    requestPermissions: vi.fn().mockResolvedValue({ location: 'granted', coarseLocation: 'granted' }),
    watchPosition: vi.fn().mockImplementation((_options: unknown, callback: (position: unknown, error: unknown) => void) => {
      callback({ coords: { latitude: 52.5, longitude: 13.4, accuracy: 10 } }, undefined);
      return Promise.resolve('watch-1');
    }),
    clearWatch: vi.fn().mockResolvedValue(undefined),
  },
}));

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((resolver) => {
    resolve = resolver;
  });
  return { promise, resolve };
}

function status(ssid: string, allowed = false): DeviceStatus {
  return {
    version: '0.4.0',
    ssid,
    uptime: 10,
    scanning: true,
    received: 100,
    dropped: 0,
    trackerPackets: 20,
    devicePackets: 30,
    devicesEnabled: true,
    locationEpoch: 0,
    alerts: 0,
    wifiClients: 1,
    freeHeap: 200000,
    homeWifi: {
      enabled: false,
      configured: false,
      connected: false,
      ssid: '',
      ip: '',
    },
    trackers: [{
      id: 'A1B2C3D4',
      type: 'Apple Find My',
      state: 'NEARBY',
      mac: 'AA:BB:CC:DD:EE:FF',
      addressType: 1,
      rssi: -60,
      peak: -55,
      sightings: 4,
      minutes: 2,
      locations: 1,
      owner: 'unknown',
      lastSeen: 1,
      allowed,
    }],
    devices: [],
  };
}

function apiMock(
  getStatus: ReturnType<typeof vi.fn>,
  extra: Record<string, ReturnType<typeof vi.fn>> = {},
): any {
  return {
    getStatus,
    setAllowed: vi.fn().mockResolvedValue(undefined),
    setHomeWifi: vi.fn().mockResolvedValue(undefined),
    markLocationMoved: vi.fn().mockResolvedValue(undefined),
    sendLocation: vi.fn().mockResolvedValue(undefined),
    ...extra,
  };
}

describe('MonitorService request ordering', () => {
  it('does not publish an old endpoint response after the address changes', async () => {
    const oldRequest = deferred<DeviceStatus>();
    const newRequest = deferred<DeviceStatus>();
    const api = apiMock(vi.fn()
      .mockReturnValueOnce(oldRequest.promise)
      .mockReturnValueOnce(newRequest.promise));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    const originalRefresh = monitor.refresh();
    const changed = monitor.setEndpoint('http://192.168.4.99');
    oldRequest.resolve(status('OLD-DEVICE'));
    await expect(originalRefresh).resolves.toBe(false);
    expect(monitor.status()).toBeNull();

    await vi.waitFor(() => expect(api.getStatus).toHaveBeenCalledTimes(2));
    expect(api.getStatus).toHaveBeenNthCalledWith(2, 'http://192.168.4.99');
    newRequest.resolve(status('NEW-DEVICE'));
    await expect(changed).resolves.toBe(true);
    expect(monitor.status()?.ssid).toBe('NEW-DEVICE');
  });

  it('starts a fresh status request after a mutation instead of joining a pre-action poll', async () => {
    const beforeAction = deferred<DeviceStatus>();
    const afterAction = deferred<DeviceStatus>();
    const api = apiMock(vi.fn()
      .mockReturnValueOnce(beforeAction.promise)
      .mockReturnValueOnce(afterAction.promise));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    const initialRefresh = monitor.refresh();
    const action = monitor.setAllowed('A1B2C3D4', true);
    expect(api.setAllowed).toHaveBeenCalledWith('http://192.168.4.1', 'A1B2C3D4', true);

    beforeAction.resolve(status('AntiStalker-TEST', false));
    await initialRefresh;
    await vi.waitFor(() => expect(api.getStatus).toHaveBeenCalledTimes(2));
    afterAction.resolve(status('AntiStalker-TEST', true));
    await action;

    expect(monitor.status()?.trackers[0].allowed).toBe(true);
    expect(monitor.acting()).toBe(false);
  });

  it('passes home Wi-Fi configuration through the shared action path', async () => {
    const api = apiMock(vi.fn().mockResolvedValue(status('AntiStalker-TEST')));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);
    const config = { enabled: true, ssid: 'HomeNetwork', password: 'secret' };

    const action = monitor.setHomeWifi(config);
    expect(monitor.acting()).toBe(true);
    await action;

    expect(api.setHomeWifi).toHaveBeenCalledWith('http://192.168.4.1', config);
    expect(api.getStatus).toHaveBeenCalledTimes(1);
    expect(monitor.acting()).toBe(false);
  });
});

describe('MonitorService trusted devices and home position', () => {
  it('runs trust actions through the shared action path', async () => {
    const api = apiMock(vi.fn().mockResolvedValue(status('AntiStalker-TEST')), {
      startTrustPairing: vi.fn().mockResolvedValue(undefined),
      cancelTrustPairing: vi.fn().mockResolvedValue(undefined),
      renameTrustDevice: vi.fn().mockResolvedValue(undefined),
      removeTrustDevice: vi.fn().mockResolvedValue(undefined),
    });
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    await monitor.startTrustPairing();
    expect(api.startTrustPairing).toHaveBeenCalledWith('http://192.168.4.1');

    await monitor.cancelTrustPairing();
    expect(api.cancelTrustPairing).toHaveBeenCalledWith('http://192.168.4.1');

    await monitor.renameTrustDevice('BC:A5:8B:1F:78:FC', 'My phone');
    expect(api.renameTrustDevice).toHaveBeenCalledWith('http://192.168.4.1', 'BC:A5:8B:1F:78:FC', 'My phone');

    await monitor.removeTrustDevice('BC:A5:8B:1F:78:FC');
    expect(api.removeTrustDevice).toHaveBeenCalledWith('http://192.168.4.1', 'BC:A5:8B:1F:78:FC');
    expect(monitor.acting()).toBe(false);
  });

  it('exposes trust and home state from the published status', async () => {
    const withTrust = status('AntiStalker-TEST');
    withTrust.trust = {
      pairing: false,
      devices: [{ name: 'android', identity: 'BC:A5:8B:1F:78:FC', addressType: 0, resolved: 2 }],
    };
    withTrust.home = {
      valid: true,
      latitude: 52.5,
      longitude: 13.4,
      accuracyM: 10,
      ageS: 5,
      source: 'phone',
      phoneDistanceM: 42,
    };
    const api = apiMock(vi.fn().mockResolvedValue(withTrust));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    await monitor.refresh();
    expect(monitor.trust()?.devices).toHaveLength(1);
    expect(monitor.home()?.phoneDistanceM).toBe(42);
  });

  it('saves and removes the home position through the shared action path', async () => {
    const api = apiMock(vi.fn().mockResolvedValue(status('AntiStalker-TEST')), {
      setHomePosition: vi.fn().mockResolvedValue(undefined),
      removeHomePosition: vi.fn().mockResolvedValue(undefined),
    });
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);
    const fix = { latitude: 52.5, longitude: 13.4, accuracy: 10, source: 'map' as const };

    await monitor.setHomePosition(fix);
    expect(api.setHomePosition).toHaveBeenCalledWith('http://192.168.4.1', fix);

    await monitor.removeHomePosition();
    expect(api.removeHomePosition).toHaveBeenCalledWith('http://192.168.4.1');
    expect(monitor.acting()).toBe(false);
  });
});

describe('MonitorService automatic device discovery', () => {
  it('falls back to a known address and adopts it when the current one fails', async () => {
    const unreachable = vi.fn()
      .mockRejectedValueOnce(new Error('The device could not be reached.'))
      .mockResolvedValueOnce(status('AntiStalker-DOWN'));
    const api = apiMock(unreachable);
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    const connected = await monitor.refresh();
    expect(connected).toBe(true);
    expect(api.getStatus).toHaveBeenNthCalledWith(2, 'http://antistalker.local', { timeoutMs: 1600 });
    expect(monitor.connected()).toBe(true);
    expect(monitor.settings().endpoint).toBe('http://antistalker.local');
    expect(monitor.status()?.ssid).toBe('AntiStalker-DOWN');
  });

  it('probes the saved home IP after switching networks', async () => {
    const atHome = status('AntiStalker-HOME');
    atHome.homeWifi = {
      enabled: true,
      configured: true,
      connected: true,
      ssid: 'HomeNetwork',
      ip: '192.168.1.42',
    };
    const api = apiMock(vi.fn()
      .mockResolvedValueOnce(atHome)               // Via hotspot: device reports its home IP.
      .mockRejectedValueOnce(new Error('down'))    // Later the hotspot address fails.
      .mockResolvedValueOnce(atHome));             // Saved home IP answers first in discovery.
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    await monitor.refresh();
    expect(monitor.settings().knownHomeIp).toBe('192.168.1.42');

    const connected = await monitor.refresh();
    expect(connected).toBe(true);
    expect(monitor.settings().endpoint).toBe('http://192.168.1.42');
  });

  it('keeps the device home IP and mDNS name for later discovery', async () => {
    const withHome = status('AntiStalker-C398');
    withHome.homeWifi = {
      enabled: true,
      configured: true,
      connected: true,
      ssid: 'HomeNetwork',
      ip: '192.168.1.42',
    };
    withHome.mdns = 'antistalker-c398.local';
    const api = apiMock(vi.fn().mockResolvedValue(withHome));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    await monitor.refresh();
    expect(monitor.settings().knownHomeIp).toBe('192.168.1.42');
    expect(monitor.settings().mdnsHost).toBe('antistalker-c398.local');
  });

  it('reports a device found by the explicit discover scan', async () => {
    const api = apiMock(vi.fn()
      .mockRejectedValueOnce(new Error('no answer'))
      .mockResolvedValueOnce(status('AntiStalker-FOUND')));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    const found = await monitor.discover();
    expect(found).toBe('http://antistalker.local');
    expect(monitor.settings().endpoint).toBe('http://antistalker.local');
    expect(monitor.connected()).toBe(true);
  });
});

describe('MonitorService phone GPS sharing', () => {
  it('starts a watch and shares the first fix while connected', async () => {
    const api = apiMock(vi.fn().mockResolvedValue(status('AntiStalker-GPS')));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);
    await monitor.refresh();

    await monitor.setLocationSharing(true);
    expect(Geolocation.watchPosition).toHaveBeenCalled();
    expect(monitor.locationSharing()).toBe(true);

    await vi.waitFor(() => expect(api.sendLocation).toHaveBeenCalledWith(
      'http://192.168.4.1',
      { latitude: 52.5, longitude: 13.4, accuracy: 10 },
    ));
    expect(monitor.locationError()).toBeNull();
  });

  it('reverts and reports when location permission is denied', async () => {
    vi.mocked(Geolocation.requestPermissions)
      .mockResolvedValueOnce({ location: 'denied', coarseLocation: 'denied' });
    const api = apiMock(vi.fn().mockResolvedValue(status('AntiStalker-GPS')));
    TestBed.configureTestingModule({ providers: [{ provide: DeviceApiService, useValue: api }] });
    const monitor = TestBed.inject(MonitorService);

    await expect(monitor.setLocationSharing(true)).rejects.toThrow(/denied/i);
    expect(monitor.locationSharing()).toBe(false);
    expect(monitor.locationError()).toMatch(/denied/i);
  });
});
