import { Component, ElementRef, Input, ViewChild, inject } from '@angular/core';
import { ModalController } from '@ionic/angular/lazy';
import * as L from 'leaflet';

/** Returns the picked coordinates to the opener via modalController.dismiss(). */
export interface HomeMapResult {
  latitude: number;
  longitude: number;
}

@Component({
  selector: 'app-home-map-modal',
  templateUrl: './home-map.modal.html',
  styleUrls: ['./home-map.modal.scss'],
  standalone: false,
})
export class HomeMapModalComponent {
  private readonly modalController = inject(ModalController);

  /** Optional starting point: the stored home position or a fresh phone fix. */
  @Input() latitude: number | null = null;
  @Input() longitude: number | null = null;

  @ViewChild('mapContainer') mapContainer?: ElementRef<HTMLElement>;

  latitudePicked: number | null = null;
  longitudePicked: number | null = null;

  private map?: L.Map;
  private marker?: L.Marker;

  get placed(): boolean {
    return this.latitudePicked != null && this.longitudePicked != null;
  }

  ionViewWillEnter(): void {
    // Wait for the modal to finish laying out. Leaflet measures its container
    // once; measure it at 0x0 and you get a grey map and a sad user.
    setTimeout(() => this.initializeMap(), 60);
  }

  ionViewWillLeave(): void {
    this.map?.remove();
    this.map = undefined;
    this.marker = undefined;
  }

  dismiss(): void {
    void this.modalController.dismiss(null);
  }

  save(): void {
    const { latitudePicked, longitudePicked } = this;
    if (latitudePicked == null || longitudePicked == null) {
      return;
    }
    void this.modalController.dismiss(
      {
        latitude: latitudePicked,
        longitude: longitudePicked,
      } satisfies HomeMapResult,
      'confirm',
    );
  }

  private initializeMap(): void {
    if (this.map || !this.mapContainer) {
      return;
    }
    const hasStart = this.latitude != null && this.longitude != null;
    this.map = L.map(this.mapContainer.nativeElement, {
      center: hasStart ? L.latLng(this.latitude!, this.longitude!) : L.latLng(20, 8),
      zoom: hasStart ? 16 : 2,
      zoomControl: true,
    });
    L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; OpenStreetMap contributors',
    }).addTo(this.map);

    if (hasStart) {
      this.placeMarker(L.latLng(this.latitude!, this.longitude!));
    }
    this.map.on('click', (event: L.LeafletMouseEvent) => this.placeMarker(event.latlng));
  }

  private placeMarker(latlng: L.LatLng): void {
    this.latitudePicked = latlng.lat;
    this.longitudePicked = latlng.lng;
    if (this.marker) {
      this.marker.setLatLng(latlng);
      return;
    }
    const icon = L.divIcon({
      className: 'home-pin-wrapper',
      html: '<span class="home-pin"></span>',
      iconSize: [26, 26],
      iconAnchor: [13, 26],
    });
    this.marker = L.marker(latlng, { icon, draggable: true }).addTo(this.map!);
    this.marker.on('dragend', () => {
      const position = this.marker!.getLatLng();
      this.latitudePicked = position.lat;
      this.longitudePicked = position.lng;
    });
  }
}
