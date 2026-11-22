import { NgModule } from '@angular/core';
import { RouterModule, Routes } from '@angular/router';

import { DevicesPage } from './devices/devices.page';
import { OverviewPage } from './overview/overview.page';
import { SetupPage } from './setup/setup.page';
import { TabsPage } from './tabs/tabs.page';

const routes: Routes = [
  {
    path: 'tabs',
    component: TabsPage,
    children: [
      { path: 'overview', component: OverviewPage },
      { path: 'devices', component: DevicesPage },
      { path: 'setup', component: SetupPage },
      { path: '', redirectTo: 'overview', pathMatch: 'full' },
    ],
  },
  { path: '', redirectTo: 'tabs/overview', pathMatch: 'full' },
  { path: '**', redirectTo: 'tabs/overview' },
];

@NgModule({
  imports: [RouterModule.forRoot(routes)],
  exports: [RouterModule],
})
export class AppRoutingModule {}
