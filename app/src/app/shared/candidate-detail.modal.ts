import { Component, Input, computed, inject } from '@angular/core';
import { Haptics, NotificationType } from '@capacitor/haptics';
import { ModalController, ToastController } from '@ionic/angular/lazy';

import { CandidateKind, candidateStateClass, displayState } from '../core/antistalker.models';
import { MonitorService } from '../core/monitor.service';

@Component({
  selector: 'app-candidate-detail',
  templateUrl: './candidate-detail.modal.html',
  styleUrls: ['./candidate-detail.modal.scss'],
  standalone: false,
})
export class CandidateDetailModalComponent {
  readonly monitor = inject(MonitorService);
  private readonly modalController = inject(ModalController);
  private readonly toastController = inject(ToastController);

  @Input({ required: true }) candidateId!: string;
  @Input({ required: true }) kind!: CandidateKind;

  readonly candidate = computed(() => {
    const status = this.monitor.status();
    const list = this.kind === 'tracker' ? status?.trackers : status?.devices;
    return list?.find((item) => item.id === this.candidateId) ?? null;
  });


  get state(): string {
    const candidate = this.candidate();
    return candidate ? displayState(candidate, this.kind) : '';
  }

  get stateClass(): string {
    const candidate = this.candidate();
    return candidate ? candidateStateClass(candidate) : 'nearby';
  }

  networkHex(value: number): string {
    return value.toString(16).padStart(2, '0').toUpperCase();
  }

  dismiss(): void {
    void this.modalController.dismiss();
  }

  async toggleAllowed(): Promise<void> {
    const candidate = this.candidate();
    if (!candidate) {
      return;
    }
    try {
      await this.monitor.setAllowed(candidate.id, !candidate.allowed);
    } catch (error) {
      await this.showToast(error instanceof Error ? error.message : 'The action could not be completed.');
      return;
    }
    void Haptics.notification({ type: NotificationType.Success }).catch(() => undefined);
    await this.showToast(candidate.allowed ? 'Alerts re-enabled for this observation.' : 'Device allowed. Alerts are now suppressed.');
    await this.modalController.dismiss();
  }

  private async showToast(message: string): Promise<void> {
    const toast = await this.toastController.create({
      message,
      duration: 3000,
      position: 'bottom',
      cssClass: 'app-toast',
    });
    await toast.present();
  }
}
