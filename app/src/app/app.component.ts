import { Component, inject } from '@angular/core';
import { addIcons } from 'ionicons';
import {
  bluetooth,
  cellular,
  checkmarkCircle,
  chevronForward,
  close,
  eye,
  eyeOff,
  footsteps,
  helpCircle,
  informationCircle,
  list,
  location,
  lockClosed,
  people,
  pulse,
  radio,
  refresh,
  settingsOutline,
  shieldCheckmark,
  sync,
  time,
  warning,
  wifi,
} from 'ionicons/icons';

import { MonitorService } from './core/monitor.service';

@Component({
  selector: 'app-root',
  templateUrl: 'app.component.html',
  styleUrls: ['app.component.scss'],
  standalone: false,
})
export class AppComponent {
  private readonly monitor = inject(MonitorService);

  constructor() {
    addIcons({
          bluetooth,
      cellular,
          checkmarkCircle,
      chevronForward,
      close,
          eye,
      eyeOff,
          footsteps,
          helpCircle,
      informationCircle,
          list,
      location,
      lockClosed,
      people,
          pulse,
      radio,
      refresh,
      settingsOutline,
      shieldCheckmark,
      sync,
      time,
      warning,
      wifi,
    });
    void this.monitor.initialize();
  }
}
