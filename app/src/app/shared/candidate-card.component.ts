import { Component, EventEmitter, Input, Output } from '@angular/core';

import {
  Candidate,
  CandidateKind,
  candidateStateClass,
  displayState,
} from '../core/antistalker.models';

@Component({
  selector: 'app-candidate-card',
  templateUrl: './candidate-card.component.html',
  standalone: false,
})
export class CandidateCardComponent {
  @Input({ required: true }) candidate!: Candidate;
  @Input({ required: true }) kind!: CandidateKind;
  @Output() readonly selected = new EventEmitter<void>();

  get state(): string {
    return displayState(this.candidate, this.kind);
  }

  get stateClass(): string {
    return candidateStateClass(this.candidate);
  }
}
