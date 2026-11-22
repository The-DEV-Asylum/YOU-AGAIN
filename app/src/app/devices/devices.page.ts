import { Component, computed, inject, signal } from '@angular/core';
import { ModalController, RefresherCustomEvent } from '@ionic/angular/lazy';

import {
  Candidate,
  CandidateFilter,
  CandidateKind,
  filterCandidates,
  isElevated,
} from '../core/antistalker.models';
import { MonitorService } from '../core/monitor.service';
import { CandidateDetailModalComponent } from '../shared/candidate-detail.modal';

@Component({
  selector: 'app-devices',
  templateUrl: './devices.page.html',
  styleUrls: ['./devices.page.scss'],
  standalone: false,
})
export class DevicesPage {
  readonly monitor = inject(MonitorService);
  private readonly modalController = inject(ModalController);

  readonly kind = signal<CandidateKind>('tracker');
  readonly query = signal('');
  readonly filter = signal<CandidateFilter>('all');

  readonly candidates = computed(() => {
    const status = this.monitor.status();
    const list = this.kind() === 'tracker' ? status?.trackers : status?.devices;
    return filterCandidates(list ?? [], this.kind(), this.query(), this.filter());
  });

  readonly source = computed(() => {
    const status = this.monitor.status();
    return this.kind() === 'tracker' ? status?.trackers ?? [] : status?.devices ?? [];
  });

  readonly attentionCount = computed(
    () => this.source().filter((candidate) => isElevated(candidate) && !candidate.allowed).length,
  );


  setKind(value: string | number | undefined): void {
    if (value === 'tracker' || value === 'device') {
      this.kind.set(value);
      this.filter.set('all');
    }
  }

  async refresh(event?: RefresherCustomEvent): Promise<void> {
    await this.monitor.refresh();
    await event?.target.complete();
  }

  async openCandidate(candidate: Candidate): Promise<void> {
    const modal = await this.modalController.create({
      component: CandidateDetailModalComponent,
      componentProps: { candidateId: candidate.id, kind: this.kind() },
      breakpoints: [0, 0.92, 1],
      initialBreakpoint: 0.92,
      cssClass: 'detail-modal',
    });
    await modal.present();
  }
}
