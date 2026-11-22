import { Pipe, PipeTransform } from '@angular/core';

import { formatLastSeen, formatUptime } from '../core/antistalker.models';

@Pipe({ name: 'relativeTime', standalone: false })
export class RelativeTimePipe implements PipeTransform {
  transform(seconds: number): string {
    return formatLastSeen(seconds);
  }
}

@Pipe({ name: 'uptime', standalone: false })
export class UptimePipe implements PipeTransform {
  transform(seconds: number): string {
    return formatUptime(seconds);
  }
}
