import { NgModule } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { BrowserModule } from '@angular/platform-browser';
import { RouteReuseStrategy } from '@angular/router';
import { IonicModule, IonicRouteStrategy } from '@ionic/angular/lazy';

import { AppRoutingModule } from './app-routing.module';
import { AppComponent } from './app.component';
import { DevicesPage } from './devices/devices.page';
import { OverviewPage } from './overview/overview.page';
import { CandidateCardComponent } from './shared/candidate-card.component';
import { CandidateDetailModalComponent } from './shared/candidate-detail.modal';
import { HomeMapModalComponent } from './shared/home-map.modal';
import { RelativeTimePipe, UptimePipe } from './shared/format.pipes';
import { SetupPage } from './setup/setup.page';
import { TabsPage } from './tabs/tabs.page';

@NgModule({
  declarations: [
    AppComponent,
    TabsPage,
    OverviewPage,
    DevicesPage,
    SetupPage,
    CandidateCardComponent,
    CandidateDetailModalComponent,
    HomeMapModalComponent,
    RelativeTimePipe,
    UptimePipe,
  ],
  imports: [BrowserModule, FormsModule, IonicModule.forRoot(), AppRoutingModule],
  providers: [{ provide: RouteReuseStrategy, useClass: IonicRouteStrategy }],
  bootstrap: [AppComponent],
})
export class AppModule {}
