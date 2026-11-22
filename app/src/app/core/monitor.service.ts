/*
 * ===========================================================================
 *  AntiStalker app :: monitor.service.ts
 *  The app's heartbeat. Polls the board, runs actions, finds it again when
 *  it wanders off to another network.
 * ===========================================================================
 *
 *  Responsibilities:
 *    - poll GET /api/status every 3/5/10 s while the app is in foreground
 *    - re-poll on resume and on network changes (hotspot <-> home Wi-Fi)
 *    - discovery: if the saved address dies, try every address the board
 *      might be living at and adopt whichever answers
 *    - opt-in GPS sharing, throttled so we do not DDoS our own ESP32
 *    - persist settings (Capacitor Preferences). Never the Wi-Fi password.
 *
 *  State is exposed as Angular signals; pages just read them.
 * ===========================================================================
 */

import { Injectable, computed, inject, signal } from '@angular/core';
import { App } from '@capacitor/app';
import { Geolocation, Position } from '@capacitor/geolocation';
import { Network } from '@capacitor/network';
import { Preferences } from '@capacitor/preferences';

import {
  AppSettings,
  ConnectionState,
  DEFAULT_SETTINGS,
  DeviceStatus,
  HomePositionFix,
  HomeWifiConfig,
  distanceMeters,
} from './antistalker.models';
import { DeviceApiError, DeviceApiService, normalizeEndpoint } from './device-api.service';

const SETTINGS_KEY = 'antistalker.settings.v1';
const CONNECTION_HELP =
  'The app automatically retries the device hotspot, its mDNS name, and its last known home IP. ' +
  'Rejoin the AntiStalker-XXXX hotspot or the same home Wi-Fi, then try again.';

// Per-address probe timeout. A live ESP32 on the LAN answers in ~100 ms;
// 1.6 s is generous and still keeps a full discovery sweep short.
const DISCOVERY_TIMEOUT_MS = 1600;
// A powered-off board must not trigger a full sweep on every refresh tick.
const DISCOVERY_THROTTLE_MS = 15_000;
// GPS forwarding: at most every 30 s, or sooner if we moved 25 m.
const GPS_SEND_MIN_INTERVAL_MS = 30_000;
const GPS_SEND_MIN_DISTANCE_M = 25;

@Injectable({ providedIn: 'root' })
export class MonitorService {
  private readonly api = inject(DeviceApiService);

  readonly status = signal<DeviceStatus | null>(null);
  readonly settings = signal<AppSettings>({ ...DEFAULT_SETTINGS });
  readonly connectionState = signal<ConnectionState>('idle');
  readonly refreshing = signal(false);
  readonly discovering = signal(false);
  readonly acting = signal(false);
  readonly lastError = signal<string | null>(null);
  readonly lastSyncedAt = signal<number | null>(null);
  readonly locationError = signal<string | null>(null);
  readonly connected = computed(() => this.connectionState() === 'connected');
  readonly locationSharing = computed(() => this.settings().locationSharing);
  readonly trust = computed(() => this.status()?.trust);
  readonly home = computed(() => this.status()?.home);

  private initialized = false;
  private active = true;
  private timer?: ReturnType<typeof setInterval>;
  private requestInFlight?: Promise<boolean>;
  private statusGeneration = 0;
  private lastDiscoveryAttemptMs = 0;
  private locationWatchId?: string;
  private lastSentFix?: { latitude: number; longitude: number };
  private lastSentFixAtMs = 0;

  async initialize(): Promise<void> {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    await this.restoreSettings();

    await App.addListener('pause', () => {
      this.active = false;
      void this.stopLocationWatch();
    });
    await App.addListener('resume', () => {
      this.active = true;
      void this.restartLocationWatchIfNeeded();
      void this.refresh();
    });
    await Network.addListener('networkStatusChange', () => {
      if (this.active) {
        void this.refresh();
      }
    });

    if (this.settings().locationSharing) {
      void this.restartLocationWatchIfNeeded();
    }
    this.restartTimer();
    await this.refresh();
  }

  async refresh(force = false): Promise<boolean> {
    if (this.requestInFlight) {
      const pending = this.requestInFlight;
      if (!force) {
        return pending;
      }
      await pending;
      return this.refresh(true);
    }

    const request = this.performRefresh(this.statusGeneration);
    this.requestInFlight = request;
    void request.finally(() => {
      if (this.requestInFlight === request) {
        this.requestInFlight = undefined;
      }
    });
    return request;
  }

  /**
   * Knocks on every door the board might be behind (current setting,
   * mDNS names, saved home IP, the hotspot default) and adopts the first one
   * that answers. Returns the working endpoint, or null if the board is
   * nowhere to be found (unplugged? out of range? eaten by the dog?).
   */
  async discover(): Promise<string | null> {
    ++this.statusGeneration;
    const generation = this.statusGeneration;
    this.discovering.set(true);
    try {
      for (const candidate of this.discoveryEndpoints()) {
        try {
          const status = await this.api.getStatus(candidate, { timeoutMs: DISCOVERY_TIMEOUT_MS });
          if (generation !== this.statusGeneration) {
            return null;
          }
          this.publishStatus(candidate, status);
          return candidate;
        } catch {
          // Try the next address.
        }
      }
      this.connectionState.set(this.status() ? 'connected' : 'disconnected');
      return null;
    } finally {
      this.discovering.set(false);
    }
  }

  async setEndpoint(value: string): Promise<boolean> {
    const endpoint = normalizeEndpoint(value);
    ++this.statusGeneration;
    const next = { ...this.settings(), endpoint };
    this.settings.set(next);
    this.status.set(null);
    this.lastSyncedAt.set(null);
    this.lastError.set(null);
    await Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    return this.refresh(true);
  }

  async setAutoRefresh(enabled: boolean): Promise<void> {
    const next = { ...this.settings(), autoRefresh: enabled };
    this.settings.set(next);
    await Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    this.restartTimer();
  }

  async setRefreshInterval(refreshInterval: number): Promise<void> {
    if (![3, 5, 10].includes(refreshInterval)) {
      return;
    }
    const next = { ...this.settings(), refreshInterval };
    this.settings.set(next);
    await Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    this.restartTimer();
  }

  /**
   * Opt in or out of sharing the phone's GPS fix with the device. While
   * enabled, the device advances its location epoch automatically after
   * genuine movement instead of waiting for a manual confirmation.
   */
  async setLocationSharing(enabled: boolean): Promise<void> {
    const next = { ...this.settings(), locationSharing: enabled };
    this.settings.set(next);
    try {
      await Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    } catch {
      // Saving prefs is best-effort. If it fails, the in-memory toggle still
      // works for this session; worst case the user flips it again tomorrow.
    }

    if (enabled) {
      try {
        await this.startLocationWatch();
      } catch (error) {
        const reverted = { ...this.settings(), locationSharing: false };
        this.settings.set(reverted);
        void Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(reverted) });
        this.locationError.set(error instanceof Error ? error.message : String(error));
        throw error;
      }
    } else {
      await this.stopLocationWatch();
    }
  }

  async markLocationMoved(): Promise<void> {
    await this.runAction(() => this.api.markLocationMoved(this.settings().endpoint));
  }

  async setAllowed(id: string, allowed: boolean): Promise<void> {
    await this.runAction(() => this.api.setAllowed(this.settings().endpoint, id, allowed));
  }

  async setHomeWifi(config: HomeWifiConfig): Promise<void> {
    await this.runAction(() => this.api.setHomeWifi(this.settings().endpoint, config));
  }

  /** Opens the BLE pairing window for trusted devices. */
  async startTrustPairing(): Promise<void> {
    await this.runAction(() => this.api.startTrustPairing(this.settings().endpoint));
  }

  async cancelTrustPairing(): Promise<void> {
    await this.runAction(() => this.api.cancelTrustPairing(this.settings().endpoint));
  }

  async renameTrustDevice(identity: string, name: string): Promise<void> {
    await this.runAction(() => this.api.renameTrustDevice(this.settings().endpoint, identity, name));
  }

  async removeTrustDevice(identity: string): Promise<void> {
    await this.runAction(() => this.api.removeTrustDevice(this.settings().endpoint, identity));
  }

  async setHomePosition(fix: HomePositionFix): Promise<void> {
    await this.runAction(() => this.api.setHomePosition(this.settings().endpoint, fix));
  }

  async removeHomePosition(): Promise<void> {
    await this.runAction(() => this.api.removeHomePosition(this.settings().endpoint));
  }

  private async performRefresh(generation: number): Promise<boolean> {
    const endpoint = this.settings().endpoint;
    this.refreshing.set(true);
    if (!this.status()) {
      this.connectionState.set('connecting');
    }
    try {
      const status = await this.api.getStatus(endpoint);
      if (this.staleRequest(generation, endpoint)) {
        return false;
      }
      this.publishStatus(endpoint, status);
      return true;
    } catch (error) {
      if (this.staleRequest(generation, endpoint)) {
        return false;
      }
      const discovered = await this.tryDiscovery(generation, endpoint);
      if (discovered !== undefined) {
        return discovered;
      }
      this.connectionState.set('disconnected');
      const detail = error instanceof DeviceApiError ? error.message : '';
      this.lastError.set(detail ? `${detail} ${CONNECTION_HELP}` : CONNECTION_HELP);
      return false;
    } finally {
      this.refreshing.set(false);
    }
  }

  /**
   * Same knocking as above, but automatic, after the saved address fails.
   * Throttled (DISCOVERY_THROTTLE_MS) so a board that is simply OFF does not
   * cause a probe storm on every refresh tick.
   */
  private async tryDiscovery(generation: number, failedEndpoint: string): Promise<boolean | undefined> {
    const now = Date.now();
    if (now - this.lastDiscoveryAttemptMs < DISCOVERY_THROTTLE_MS) {
      return undefined;
    }
    this.lastDiscoveryAttemptMs = now;
    this.discovering.set(true);
    try {
      for (const candidate of this.discoveryEndpoints()) {
        if (candidate === failedEndpoint) {
          continue;
        }
        try {
          const status = await this.api.getStatus(candidate, { timeoutMs: DISCOVERY_TIMEOUT_MS });
          if (this.staleRequest(generation, failedEndpoint)) {
            return false;
          }
          this.publishStatus(candidate, status);
          return true;
        } catch {
          // Try the next address.
        }
      }
    } finally {
      this.discovering.set(false);
    }
    return undefined;
  }

  /** Ordered, de-duplicated list of addresses where the device may live. */
  private discoveryEndpoints(): string[] {
    const settings = this.settings();
    const candidates = [
      settings.endpoint,
      // Remembered addresses first: while the board is on home Wi-Fi its
      // hotspot is OFF, so these are the most likely to answer.
      settings.mdnsHost ? `http://${settings.mdnsHost}` : undefined, // antistalker-xxxx.local
      settings.knownHomeIp ? `http://${settings.knownHomeIp}` : undefined,
      DEFAULT_SETTINGS.endpoint, // Fallback hotspot, always 192.168.4.1.
      'http://antistalker.local',
    ];
    return [...new Set(candidates.filter((value): value is string => Boolean(value)))];
  }

  private staleRequest(generation: number, endpoint: string): boolean {
    return generation !== this.statusGeneration || endpoint !== this.settings().endpoint;
  }

  private publishStatus(endpoint: string, status: DeviceStatus): void {
    if (endpoint !== this.settings().endpoint) {
      // The board answered from a different address. Adopt it silently,
      // the user should never have to type an IP twice.
      const next = { ...this.settings(), endpoint };
      this.settings.set(next);
      void Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    }
    this.rememberDeviceAddresses(status);
    this.status.set(status);
    this.connectionState.set('connected');
    this.lastError.set(null);
    this.lastSyncedAt.set(Date.now());
  }

  /** Keep the device's home IP and mDNS name so discovery can use them later. */
  private rememberDeviceAddresses(status: DeviceStatus): void {
    const homeIp = status.homeWifi.ip;
    const mdnsHost = status.mdns;
    let changed = false;
    const next = { ...this.settings() };
    if (homeIp && next.knownHomeIp !== homeIp) {
      next.knownHomeIp = homeIp;
      changed = true;
    }
    if (mdnsHost && next.mdnsHost !== mdnsHost) {
      next.mdnsHost = mdnsHost;
      changed = true;
    }
    if (changed) {
      this.settings.set(next);
      void Preferences.set({ key: SETTINGS_KEY, value: JSON.stringify(next) });
    }
  }

  private async runAction(action: () => Promise<void>): Promise<void> {
    if (this.acting()) {
      return;
    }
    this.acting.set(true);
    try {
      await action();
      await this.refresh(true);
    } catch (error) {
      if (error instanceof DeviceApiError) {
        throw error;
      }
      throw new DeviceApiError('The device did not complete the action. Check the Wi-Fi connection and try again.');
    } finally {
      this.acting.set(false);
    }
  }

  private async restoreSettings(): Promise<void> {
    try {
      const { value } = await Preferences.get({ key: SETTINGS_KEY });
      if (!value) {
        return;
      }
      const saved = JSON.parse(value) as Partial<AppSettings>;
      this.settings.set({
        endpoint: normalizeEndpoint(saved.endpoint ?? DEFAULT_SETTINGS.endpoint),
        autoRefresh: saved.autoRefresh !== false,
        refreshInterval: [3, 5, 10].includes(saved.refreshInterval ?? 0)
          ? (saved.refreshInterval as number)
          : DEFAULT_SETTINGS.refreshInterval,
        knownHomeIp: typeof saved.knownHomeIp === 'string' && saved.knownHomeIp
          ? saved.knownHomeIp
          : undefined,
        mdnsHost: typeof saved.mdnsHost === 'string' && saved.mdnsHost
          ? saved.mdnsHost
          : undefined,
        locationSharing: saved.locationSharing === true,
      });
    } catch {
      this.settings.set({ ...DEFAULT_SETTINGS });
    }
  }

  private restartTimer(): void {
    if (this.timer) {
      clearInterval(this.timer);
    }
    this.timer = setInterval(() => {
      if (this.active && this.settings().autoRefresh) {
        void this.refresh();
      }
    }, this.settings().refreshInterval * 1000);
  }

  private async restartLocationWatchIfNeeded(): Promise<void> {
    if (this.settings().locationSharing) {
      await this.startLocationWatch();
    }
  }

  private async startLocationWatch(): Promise<void> {
    if (this.locationWatchId !== undefined) {
      return;
    }
    try {
      const permissions = await Geolocation.requestPermissions();
      const granted = permissions.location === 'granted' ||
        permissions.coarseLocation === 'granted';
      if (!granted && permissions.location !== 'prompt') {
        throw new Error(
          'Location permission was denied. Enable it for AntiStalker in the system settings to share GPS.',
        );
      }
    } catch (error) {
      // Some platforms only show the permission prompt once the watch starts.
      // A real "no" comes back through the watch callback below anyway.
      if (error instanceof Error && error.message.includes('denied')) {
        throw error;
      }
    }

    this.locationError.set(null);
    this.locationWatchId = await Geolocation.watchPosition(
      { enableHighAccuracy: true, timeout: 20_000, maximumAge: 10_000 },
      (position, watchError) => {
        if (watchError) {
          this.locationError.set(
            watchError instanceof Error ? watchError.message : 'Phone location is unavailable.',
          );
          return;
        }
        if (position) {
          void this.maybeSendLocation(position);
        }
      },
    );
  }

  private async stopLocationWatch(): Promise<void> {
    if (this.locationWatchId === undefined) {
      return;
    }
    const id = this.locationWatchId;
    this.locationWatchId = undefined;
    try {
      await Geolocation.clearWatch({ id });
    } catch {
      // The watch may already be gone. Nothing to clean, move along.
    }
  }

  /**
   * Forwards the newest GPS fix to the board, throttled by time AND distance,
   * so a phone sitting on a table does not hammer a single-threaded web
   * server running on an 80 MHz microcontroller.
   */
  private async maybeSendLocation(position: Position): Promise<void> {
    if (!this.connected()) {
      return;
    }
    const coords = position.coords;
    if (!Number.isFinite(coords.latitude) || !Number.isFinite(coords.longitude)) {
      return;
    }
    const now = Date.now();
    const movedMeters = this.lastSentFix
      ? distanceMeters(this.lastSentFix.latitude, this.lastSentFix.longitude, coords.latitude, coords.longitude)
      : Number.POSITIVE_INFINITY;
    if (this.lastSentFix && now - this.lastSentFixAtMs < GPS_SEND_MIN_INTERVAL_MS &&
        movedMeters < GPS_SEND_MIN_DISTANCE_M) {
      return;
    }

    try {
      await this.api.sendLocation(this.settings().endpoint, {
        latitude: coords.latitude,
        longitude: coords.longitude,
        accuracy: Number.isFinite(coords.accuracy) ? coords.accuracy : undefined,
      });
      this.lastSentFix = { latitude: coords.latitude, longitude: coords.longitude };
      this.lastSentFixAtMs = now;
      this.locationError.set(null);
    } catch (error) {
      const detail = error instanceof DeviceApiError ? error.message : '';
      this.locationError.set(detail || 'The device did not accept the location update.');
    }
  }
}
