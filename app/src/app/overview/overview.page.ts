import { Component, computed, inject } from '@angular/core';
import { Haptics, NotificationType } from '@capacitor/haptics';
import { AlertController, ModalController, RefresherCustomEvent, ToastController } from '@ionic/angular/lazy';

import { Candidate, CandidateKind, activeCount, formatLastSeen, isElevated } from '../core/antistalker.models';
import { MonitorService } from '../core/monitor.service';
import { CandidateDetailModalComponent } from '../shared/candidate-detail.modal';

@Component({
  selector: 'app-overview',
  templateUrl: './overview.page.html',
  styleUrls: ['./overview.page.scss'],
  standalone: false,
})
export class OverviewPage {
  readonly monitor = inject(MonitorService);
  private readonly alertController = inject(AlertController);
  private readonly modalController = inject(ModalController);
  private readonly toastController = inject(ToastController);

  readonly attention = computed(() => {
    const status = this.monitor.status();
    if (!status) {
      return [] as { candidate: Candidate; kind: CandidateKind }[];
    }
    return [
      ...status.trackers.map((candidate) => ({ candidate, kind: 'tracker' as const })),
      ...status.devices.map((candidate) => ({ candidate, kind: 'device' as const })),
    ]
      .filter(({ candidate }) => isElevated(candidate) && !candidate.allowed)
      .sort((a, b) => a.candidate.lastSeen - b.candidate.lastSeen);
  });

  /** Advertisers that actually transmitted within the last two minutes. */
  readonly activeTrackers = computed(() => {
    const status = this.monitor.status();
    return status ? activeCount(status.trackers) : 0;
  });

  readonly activeDevices = computed(() => {
    const status = this.monitor.status();
    return status ? activeCount(status.devices) : 0;
  });

  /** True when the device advances epochs automatically from phone GPS. */
  readonly gpsEpochs = computed(() => this.monitor.locationSharing());

  readonly heroExtras = computed(() => {
    const status = this.monitor.status();
    const extras: { icon: string; text: string }[] = [];
    if (!status) {
      return extras;
    }
    if (status.temperatureC != null) {
      extras.push({ icon: 'thermometer', text: `${status.temperatureC.toFixed(1)} °C` });
    }
    const battery = status.battery;
    if (battery?.available && battery.millivolts) {
      const volts = (battery.millivolts / 1000).toFixed(2);
      extras.push({
        icon: 'battery-half',
        text: battery.percent != null ? `${volts} V · ${battery.percent}%` : `${volts} V`,
      });
    }
    const trusted = status.trust?.devices.length ?? 0;
    if (trusted > 0) {
      extras.push({ icon: 'shield-checkmark', text: `${trusted} trusted` });
    }
    return extras;
  });

  /** "Phone is ~N m from home" context; only meaningful while a fix exists. */
  readonly homeNote = computed(() => {
    const home = this.monitor.status()?.home;
    if (!home?.valid || home.phoneDistanceM == null) {
      return null;
    }
    return `Your phone is about ${Math.round(home.phoneDistanceM)} m from your saved home position.`;
  });

  readonly lastFixText = computed(() => {
    const location = this.monitor.status()?.phoneLocation;
    if (!location?.valid) {
      return null;
    }
    const accuracy = location.accuracyM > 0 ? ` ±${Math.round(location.accuracyM)} m` : '';
    return `Device has your GPS fix (${formatLastSeen(location.ageS)})${accuracy}`;
  });

  async refresh(event?: RefresherCustomEvent): Promise<void> {
    await this.monitor.refresh();
    await event?.target.complete();
  }

  async openCandidate(candidate: Candidate, kind: CandidateKind): Promise<void> {
    const modal = await this.modalController.create({
      component: CandidateDetailModalComponent,
      componentProps: { candidateId: candidate.id, kind },
      breakpoints: [0, 0.92, 1],
      initialBreakpoint: 0.92,
      cssClass: 'detail-modal',
    });
    await modal.present();
  }

  async confirmLocation(): Promise<void> {
    const alert = await this.alertController.create({
      header: 'Confirm a new location',
      message: 'Only continue after genuinely changing physical location. This starts a new observation epoch for both trackers and nearby BLE devices.',
      cssClass: 'app-alert',
      buttons: [
        { text: 'Cancel', role: 'cancel' },
        { text: 'I have moved', role: 'confirm', cssClass: 'alert-confirm' },
      ],
    });
    await alert.present();
    const { role } = await alert.onDidDismiss();
    if (role !== 'confirm') {
      return;
    }

    try {
      await this.monitor.markLocationMoved();
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'Location could not be updated.');
      return;
    }
    void Haptics.notification({ type: NotificationType.Success }).catch(() => undefined);
    await this.showToast('New location confirmed. Keep monitoring as you move.');
  }

  private async showToast(message: string): Promise<void> {
    const toast = await this.toastController.create({
      message,
      duration: 3200,
      position: 'bottom',
      cssClass: 'app-toast',
    });
    await toast.present();
  }
}
