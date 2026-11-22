import { TestBed } from '@angular/core/testing';
import { vi } from 'vitest';

import { AppComponent } from './app.component';
import { AppModule } from './app.module';
import { MonitorService } from './core/monitor.service';

describe('AppComponent', () => {
  beforeEach(async () => {
    await TestBed.configureTestingModule({
      imports: [AppModule],
      providers: [{
        provide: MonitorService,
        useValue: { initialize: vi.fn().mockResolvedValue(undefined) },
      }],
    }).compileComponents();
  });

  it('creates the app and starts monitoring', () => {
    const fixture = TestBed.createComponent(AppComponent);
    const app = fixture.componentInstance;
    expect(app).toBeTruthy();
    expect(TestBed.inject(MonitorService).initialize).toHaveBeenCalledOnce();
  });
});
