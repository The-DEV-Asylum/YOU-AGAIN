import { Component, computed, effect, inject } from '@angular/core';
import { Geolocation } from '@capacitor/geolocation';
import { AlertController, ModalController, RefresherCustomEvent, ToastController } from '@ionic/angular/lazy';

import { DEFAULT_SETTINGS, TrustDevice, formatBytes, formatUptime } from '../core/antistalker.models';
import { MonitorService } from '../core/monitor.service';
import { HomeMapModalComponent } from '../shared/home-map.modal';

@Component({
  selector: 'app-setup',
  templateUrl: './setup.page.html',
  styleUrls: ['./setup.page.scss'],
  standalone: false,
})
export class SetupPage {
  readonly monitor = inject(MonitorService);
  private readonly toastController = inject(ToastController);
  private readonly alertController = inject(AlertController);
  private readonly modalController = inject(ModalController);

  /** Trusted-device and home state from the firmware (absent on older firmware). */
  readonly trust = this.monitor.trust;
  readonly home = this.monitor.home;

  endpoint = this.monitor.settings().endpoint;
  homeWifiEnabled = false;
  homeWifiSsid = '';
  homeWifiPassword = '';
  readonly defaultEndpoint = DEFAULT_SETTINGS.endpoint;
  private endpointDirty = false;
  private homeWifiSsidDirty = false;

  /** Rows for the device health/system section; adapts to firmware fields. */
  readonly systemRows = computed(() => {
    const status = this.monitor.status();
    if (!status) {
      return [] as { label: string; value: string }[];
    }
    const battery = status.battery;
    const batteryValue = battery?.available && battery.millivolts
      ? `${(battery.millivolts / 1000).toFixed(2)} V${battery.percent != null ? ` · ${battery.percent}%` : ''}`
      : 'Not available';
    return [
      { label: 'Firmware', value: status.version },
      { label: 'BLE scanner', value: status.scanning ? 'Scanning' : 'Stopped' },
      { label: 'General BLE monitoring', value: status.devicesEnabled ? 'Enabled' : 'Paused' },
      { label: 'Uptime', value: formatUptime(status.uptime) },
      { label: 'Chip', value: status.chipModel || 'Unknown' },
      {
        label: 'Free heap',
        value: status.totalHeap
          ? `${formatBytes(status.freeHeap)} of ${formatBytes(status.totalHeap)}`
          : formatBytes(status.freeHeap),
      },
      {
        label: 'Free PSRAM',
        value: status.freePsram !== undefined ? formatBytes(status.freePsram) : 'Unknown',
      },
      {
        label: 'Free app space',
        value: status.flashFree !== undefined ? formatBytes(status.flashFree) : 'Unknown',
      },
      {
        label: 'Chip temperature',
        value: status.temperatureC != null ? `${status.temperatureC.toFixed(1)} °C` : 'Not available',
      },
      { label: 'Battery', value: batteryValue },
      { label: 'mDNS name', value: status.mdns || 'Not available' },
      { label: 'Hotspot clients', value: String(status.wifiClients) },
      {
        label: 'Candidate slots',
        value: status.maxCandidates
          ? `${status.trackers.length} tags · ${status.devices.length} devices of ${status.maxCandidates}`
          : 'Unknown',
      },
    ];
  });

  constructor() {
    effect(() => {
      if (!this.endpointDirty) {
        this.endpoint = this.monitor.settings().endpoint;
      }
    });
    effect(() => {
      const status = this.monitor.status();
      if (!status) {
        return;
      }
      this.homeWifiEnabled = status.homeWifi.enabled;
      if (!this.homeWifiSsidDirty) {
        this.homeWifiSsid = status.homeWifi.ssid;
      }
    });
  }

  markDirty(): void {
    this.endpointDirty = true;
  }

  markHomeWifiDirty(): void {
    this.homeWifiSsidDirty = true;
  }

  async connect(): Promise<void> {
    try {
      const connected = await this.monitor.setEndpoint(this.endpoint);
      this.endpointDirty = false;
      this.endpoint = this.monitor.settings().endpoint;
      await this.showToast(connected ? 'Connected to AntiStalker.' : 'Not connected. Check the Wi-Fi steps below.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'Check the device address.');
    }
  }

  /** Probe the hotspot, mDNS and saved home addresses and adopt the first hit. */
  async findDevice(): Promise<void> {
    const endpoint = await this.monitor.discover();
    this.endpointDirty = false;
    this.endpoint = this.monitor.settings().endpoint;
    if (endpoint) {
      await this.showToast(`Found the device at ${endpoint}. Address saved.`);
    } else {
      await this.showToast('No AntiStalker answered. Power it on, join its Wi-Fi, and try again.');
    }
  }

  async refresh(event?: RefresherCustomEvent): Promise<void> {
    await this.monitor.refresh();
    await event?.target.complete();
  }

  async useDefaultAddress(): Promise<void> {
    this.endpoint = this.defaultEndpoint;
    this.endpointDirty = false;
    await this.connect();
  }

  async setHomeWifiEnabled(enabled: boolean): Promise<void> {
    const previous = this.monitor.status()?.homeWifi.enabled ?? !enabled;
    this.homeWifiEnabled = enabled;
    if (!this.monitor.connected() || this.monitor.acting()) {
      this.homeWifiEnabled = previous;
      return;
    }

    try {
      await this.monitor.setHomeWifi({ enabled });
      await this.showToast(enabled
        ? 'Home Wi-Fi enabled. Your current app connection may drop.'
        : 'Home Wi-Fi disabled. Rejoin the AntiStalker hotspot if disconnected.');
    } catch (error) {
      this.homeWifiEnabled = previous;
      await this.showToast(error instanceof Error ? error.message : 'Home Wi-Fi could not be changed.');
    }
  }

  async setLocationSharing(enabled: boolean): Promise<void> {
    try {
      await this.monitor.setLocationSharing(enabled);
      await this.showToast(enabled
        ? 'Sharing GPS with the device. Location epochs now advance automatically as you move.'
        : 'Phone GPS sharing is off.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'Location sharing could not be enabled.');
    }
  }

  async saveHomeWifi(): Promise<void> {
    if (!this.monitor.connected() || this.monitor.acting()) {
      return;
    }
    if (!this.homeWifiSsid.trim()) {
      await this.showToast('Enter the home Wi-Fi network name.');
      return;
    }

    const enabled = this.homeWifiEnabled;
    try {
      await this.monitor.setHomeWifi({
        enabled,
        ssid: this.homeWifiSsid,
        password: this.homeWifiPassword,
      });
      this.homeWifiSsidDirty = false;
      this.homeWifiPassword = '';
      await this.showToast(enabled
        ? 'Home network saved on the ESP32. Your current app connection may drop.'
        : 'Home network saved on the ESP32.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'Home Wi-Fi could not be saved.');
    }
  }

  // ------------------------------------------------------------------
  // Trusted devices (one-time BLE pairing)
  // The board shows the passkey, the phone types it. See the firmware's
  // TrustManager.h for the full dance.
  // ------------------------------------------------------------------

  /** Opens the pairing window; the passkey arrives with the next status poll. */
  async startPairing(): Promise<void> {
    if (!this.monitor.connected() || this.monitor.acting()) {
      return;
    }
    try {
      await this.monitor.startTrustPairing();
      await this.monitor.refresh(true);
      await this.showToast('Pairing window open. On the device: Settings > Bluetooth > tap AntiStalker, then enter the code shown here.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The pairing window could not be opened.');
    }
  }

  async cancelPairing(): Promise<void> {
    if (this.monitor.acting()) {
      return;
    }
    try {
      await this.monitor.cancelTrustPairing();
      await this.showToast('Pairing window closed. Scanning resumed.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The pairing window could not be closed.');
    }
  }

  async renameTrust(device: TrustDevice): Promise<void> {
    let newName = '';
    const alert = await this.alertController.create({
      header: 'Rename trusted device',
      inputs: [
        { name: 'name', type: 'text', value: device.name, placeholder: 'e.g. My phone' },
      ],
      buttons: [
        { text: 'Cancel', role: 'cancel' },
        {
          text: 'Save',
          role: 'confirm',
          handler: (values: { name?: string }) => {
            newName = (values?.name ?? '').trim();
            return newName.length > 0;
          },
        },
      ],
      cssClass: 'app-alert',
    });
    await alert.present();
    const { role } = await alert.onDidDismiss();
    if (role !== 'confirm' || !newName || newName === device.name) {
      return;
    }

    try {
      await this.monitor.renameTrustDevice(device.identity, newName);
      await this.showToast(`Renamed to “${newName}”.`);
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The device could not be renamed.');
    }
  }

  async removeTrust(device: TrustDevice): Promise<void> {
    const alert = await this.alertController.create({
      header: 'Remove trusted device?',
      message: `“${device.name}” will no longer be recognized and may raise risk signals again. The stored Bluetooth bond is deleted too.`,
      cssClass: 'app-alert',
      buttons: [
        { text: 'Keep', role: 'cancel' },
        { text: 'Remove', role: 'destructive', cssClass: 'alert-confirm' },
      ],
    });
    await alert.present();
    const { role } = await alert.onDidDismiss();
    if (role !== 'destructive') {
      return;
    }

    try {
      await this.monitor.removeTrustDevice(device.identity);
      await this.showToast(`Removed ${device.name}. Pair it again anytime.`);
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The device could not be removed.');
    }
  }

  // ------------------------------------------------------------------
  // Home position (context only, never silences anything)
  // ------------------------------------------------------------------

  /** Stores this phone's current GPS fix as the home position. */
  async setHomeFromPhone(): Promise<void> {
    if (!this.monitor.connected() || this.monitor.acting()) {
      return;
    }
    try {
      const position = await Geolocation.getCurrentPosition({ enableHighAccuracy: true, timeout: 15_000 });
      const { latitude, longitude } = position.coords;
      if (!Number.isFinite(latitude) || !Number.isFinite(longitude)) {
        throw new Error('No usable GPS fix right now.');
      }
      await this.monitor.setHomePosition({
        latitude,
        longitude,
        accuracy: Number.isFinite(position.coords.accuracy) ? position.coords.accuracy : undefined,
        source: 'phone',
      });
      await this.showToast('Home position saved from this phone’s GPS.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The home position could not be saved.');
    }
  }

  /** Opens the draggable map and saves the picked coordinates on save. */
  async openHomeMap(): Promise<void> {
    if (!this.monitor.connected()) {
      return;
    }
    const home = this.home();
    const phone = this.monitor.status()?.phoneLocation;
    const start = home?.valid
      ? { latitude: home.latitude, longitude: home.longitude }
      : phone?.valid
        ? { latitude: phone.latitude, longitude: phone.longitude }
        : null;

    const modal = await this.modalController.create({
      component: HomeMapModalComponent,
      componentProps: {
        latitude: start?.latitude ?? null,
        longitude: start?.longitude ?? null,
      },
      breakpoints: [0, 0.9, 1],
      initialBreakpoint: 0.9,
      cssClass: 'detail-modal',
    });
    await modal.present();
    const { data, role } = await modal.onDidDismiss<{ latitude: number; longitude: number }>();
    if (role !== 'confirm' || !data) {
      return;
    }

    try {
      await this.monitor.setHomePosition({ ...data, source: 'map' });
      await this.showToast('Home position saved from the map.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The home position could not be saved.');
    }
  }

  async clearHome(): Promise<void> {
    const alert = await this.alertController.create({
      header: 'Remove home position?',
      message: 'The “phone is ~N m from home” note disappears. Alerts are not affected; they never were.',
      cssClass: 'app-alert',
      buttons: [
        { text: 'Keep', role: 'cancel' },
        { text: 'Remove', role: 'destructive', cssClass: 'alert-confirm' },
      ],
    });
    await alert.present();
    const { role } = await alert.onDidDismiss();
    if (role !== 'destructive') {
      return;
    }

    try {
      await this.monitor.removeHomePosition();
      await this.showToast('Home position removed.');
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The home position could not be removed.');
    }
  }

  private async showToast(message: string): Promise<void> {
    const toast = await this.toastController.create({
      message,
      duration: 3500,
      position: 'bottom',
      cssClass: 'app-toast',
    });
    await toast.present();
  }
}
