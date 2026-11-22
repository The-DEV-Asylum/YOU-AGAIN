/*
 * ===========================================================================
 *  AntiStalker :: WebPortal.cpp
 *  Wi-Fi juggling, the embedded dashboard and every /api route.
 * ===========================================================================
 *
 *  Map of this file, top to bottom:
 *    1. tunables (retry timers, auto-epoch distance, battery divider)
 *    2. tiny helpers (haversine, battery estimate, password generator)
 *    3. kIndexPage: the whole dashboard, HTML + CSS + JS in one raw string.
 *       No CDN, no framework, works with zero internet. Ugly to edit, lovely
 *       to deploy.
 *    4. WebPortal: Wi-Fi state machine, routes, JSON builders, handlers
 *
 *  Every handler validates its input like it came from a stranger, because
 *  on a shared network it might have. Bad JSON -> 400, missing header -> 403.
 * ===========================================================================
 */

#include "WebPortal.h"

#include "BleMonitor.h"

#include "DistanceEstimate.h"
#include "TrustManager.h"
#include "TrustResolver.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_system.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr uint32_t kHomeRetryIntervalMs = 30UL * 1000UL;
constexpr uint32_t kHomeReconnectDelayMs = 1000UL;
constexpr uint32_t kApRetryIntervalMs = 5UL * 1000UL;
// How far the phone must move (by its own GPS) before the board starts a new
// location epoch by itself. 150 m: more than "went to the kitchen", less than
// "took the bus two stops". Moving rooms must NOT count as a new location,
// otherwise your own flatmate's phone becomes a "follower".
constexpr float kAutoEpochDistanceM = 150.0f;
// GPS fixes worse than this are shown but never advance the epoch. Indoors,
// a phone with 300 m accuracy happily "teleports" across the block while you
// sit on the sofa. That is not movement, that is GPS having a bad day.
constexpr float kAutoEpochMaxAccuracyM = 100.0f;

#ifndef ANTISTALKER_BATTERY_PIN
// Battery sensing is optional: the bare ESP32-S3 dev board has no fuel gauge
// and no idea it is even on a battery. To enable it, define (in
// platformio.ini) ANTISTALKER_BATTERY_PIN (ADC GPIO) and
// ANTISTALKER_BATTERY_DIVIDER (battery volts / pin volts, 2.0 for 100k/100k).
// Keep the pin below ~3.1 V at full charge or the ADC clips (or worse).
#define ANTISTALKER_BATTERY_PIN -1
#endif
#ifndef ANTISTALKER_BATTERY_DIVIDER
#define ANTISTALKER_BATTERY_DIVIDER 2.0f
#endif
constexpr int kBatteryPin = ANTISTALKER_BATTERY_PIN;

// Linear 1S Li-ion "percentage". Real cells have a curvy discharge profile,
// so treat this number like a weather forecast: roughly right, often not.
constexpr uint32_t kBatteryEmptyMv = 3500;
constexpr uint32_t kBatteryFullMv = 4200;

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6371000.0;

double toRadians(double degrees) {
    return degrees * kPi / 180.0;
}

// Haversine great-circle distance. Overkill for 150 m? Yes. Correct? Also yes.
float distanceMeters(double lat1, double lon1, double lat2, double lon2) {
    const double dLat = toRadians(lat2 - lat1);
    const double dLon = toRadians(lon2 - lon1);
    const double a = sin(dLat / 2.0) * sin(dLat / 2.0) +
                     cos(toRadians(lat1)) * cos(toRadians(lat2)) *
                         sin(dLon / 2.0) * sin(dLon / 2.0);
    return static_cast<float>(2.0 * kEarthRadiusM * asin(sqrt(a)));
}

void initBatterySensor() {
    if (kBatteryPin < 0) {
        return;
    }
    analogSetPinAttenuation(static_cast<uint8_t>(kBatteryPin), ADC_11db);
}

uint32_t readBatteryMillivolts() {
    if (kBatteryPin < 0) {
        return 0;
    }
    const uint32_t pinMillivolts = analogReadMilliVolts(static_cast<uint8_t>(kBatteryPin));
    return static_cast<uint32_t>(pinMillivolts * ANTISTALKER_BATTERY_DIVIDER + 0.5f);
}

uint8_t batteryPercentEstimate(uint32_t millivolts) {
    if (millivolts <= kBatteryEmptyMv) {
        return 0;
    }
    if (millivolts >= kBatteryFullMv) {
        return 100;
    }
    const uint32_t span = kBatteryFullMv - kBatteryEmptyMv;
    return static_cast<uint8_t>(((millivolts - kBatteryEmptyMv) * 100UL + span / 2) / span);
}

const char kIndexPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AntiStalker</title>
<style>
:root{color-scheme:dark;--bg:#10151c;--panel:#19222d;--text:#e8eef5;--muted:#9cacbd;--accent:#54c7a4;--warn:#ffcc66;--danger:#ff7878}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,sans-serif}
main{max-width:1100px;margin:auto;padding:20px}h1{margin:0;font-size:28px}h2{margin-top:26px;font-size:20px}
.warning,.card,section{background:var(--panel);border-radius:12px;padding:14px;margin-top:14px}.warning{border-left:4px solid var(--warn)}
.stats{display:flex;flex-wrap:wrap;gap:14px;color:var(--muted)}.stats b{color:var(--text)}
button{border:0;border-radius:8px;padding:10px 14px;background:var(--accent);color:#092018;font-weight:700;cursor:pointer}button.secondary{background:#334355;color:var(--text);padding:6px 9px}
#moved{margin-top:12px}.table-wrap{overflow-x:auto}table{width:100%;border-collapse:collapse;white-space:nowrap}th,td{text-align:left;padding:8px;border-bottom:1px solid #2c3948}th{color:var(--muted)}
th.sortable{cursor:pointer;user-select:none}th.sortable:hover{color:var(--text)}th.sortable .ind{font-style:normal;font-size:9px;margin-left:5px;color:var(--accent)}
.state{font-weight:700}.following{color:var(--danger)}.persistent{color:var(--warn)}.nearby{color:var(--accent)}.muted{color:var(--muted)}
.trustBadge{display:inline-block;background:var(--accent);color:#092018;font-size:10px;font-weight:700;border-radius:5px;padding:1px 6px;margin-right:6px;vertical-align:1px}
.passkey{font:700 34px/1.2 ui-monospace,monospace;letter-spacing:4px;margin:10px 0}
.homeRow{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-top:10px}
#status{min-height:22px;color:var(--muted)}code{color:var(--accent)}
.search{width:100%;max-width:420px;margin:4px 0 12px;padding:10px 12px;border:1px solid #3a4a5e;border-radius:8px;background:#10151c;color:var(--text);font:inherit}.search:focus{outline:2px solid var(--accent);border-color:transparent}
</style>
</head>
<body><main>
<h1>AntiStalker</h1><div class="muted">Local-only BLE monitor · <span id="ssid"></span></div>
<div class="warning"><b>Risk signals, not accusations.</b> A nearby or co-present device does not prove stalking. RSSI is not distance. Confirm your own devices and inspect your belongings safely.</div>
<div class="card"><div class="stats"><span>Uptime <b id="uptime">–</b></span><span>BLE <b id="radio">–</b></span><span>Packets <b id="packets">–</b></span><span>Dropped <b id="dropped">–</b></span><span>Location <b id="epoch">–</b></span><span>Alerts <b id="alerts">–</b></span><span>Memory <b id="heap">–</b></span><span>Temp <b id="temp">–</b></span><span>Battery <b id="battery">–</b></span><span>Phone GPS <b id="gps">–</b></span><span>Trusted <b id="trustedCount">–</b></span></div>
<button id="moved">I am at a genuinely different location</button><div id="status" role="status"></div></div>
<section><h2>Trusted devices</h2><p class="muted">Pair your own phone/watch once: AntiStalker then recognizes the rotating addresses it transmits (Bluetooth identity keys), shows it by name, and never raises alerts for it. Strangers cannot add themselves — the pairing code is only shown on your own screen.</p><div id="pairBox"></div><div class="table-wrap"><table><thead><tr><th>Name</th><th>Identity address</th><th>Recognized</th><th></th></tr></thead><tbody id="trustRows"></tbody></table></div><button id="pairBtn">Pair a device</button></section>
<section><h2>Home position</h2><div id="homeBox"></div></section>
<section><h2>Tracking tags</h2><input id="trackersSearch" class="search" type="search" placeholder="Search tags by ID, type, state, MAC…" aria-label="Search tracking tags"><div class="table-wrap"><table><thead><tr><th class="sortable" data-sort="id" title="Sort by ID">ID<i class="ind"></i></th><th class="sortable" data-sort="type" title="Sort by type">Type<i class="ind"></i></th><th class="sortable" data-sort="state" title="Sort by state">State<i class="ind"></i></th><th class="sortable" data-sort="mac" title="Sort by MAC">MAC<i class="ind"></i></th><th class="sortable" data-sort="rssi" title="Sort by signal strength">RSSI / peak<i class="ind"></i></th><th class="sortable" data-sort="distanceM" title="Sort by rough distance estimate">Dist<i class="ind"></i></th><th class="sortable" data-sort="sightings" title="Sort by sightings">Seen<i class="ind"></i></th><th class="sortable" data-sort="locations" title="Sort by locations">Places<i class="ind"></i></th><th class="sortable" data-sort="lastSeen" title="Sort by last seen">Last<i class="ind"></i></th><th></th></tr></thead><tbody id="trackers"></tbody></table></div></section>
<section><h2>Phones / wearables / other BLE</h2><p class="muted">CO-PRESENT means the same MAC address appeared repeatedly at multiple confirmed locations. Address rotation can hide a device; shared addresses and spoofing can mislead.</p><input id="devicesSearch" class="search" type="search" placeholder="Search devices by ID, type, state, MAC…" aria-label="Search personal and general BLE devices"><div class="table-wrap"><table><thead><tr><th class="sortable" data-sort="id" title="Sort by ID">ID<i class="ind"></i></th><th class="sortable" data-sort="type" title="Sort by type">Type<i class="ind"></i></th><th class="sortable" data-sort="state" title="Sort by state">State<i class="ind"></i></th><th class="sortable" data-sort="mac" title="Sort by MAC">MAC<i class="ind"></i></th><th class="sortable" data-sort="rssi" title="Sort by signal strength">RSSI / peak<i class="ind"></i></th><th class="sortable" data-sort="distanceM" title="Sort by rough distance estimate">Dist<i class="ind"></i></th><th class="sortable" data-sort="sightings" title="Sort by sightings">Seen<i class="ind"></i></th><th class="sortable" data-sort="locations" title="Sort by locations">Places<i class="ind"></i></th><th class="sortable" data-sort="lastSeen" title="Sort by last seen">Last<i class="ind"></i></th><th></th></tr></thead><tbody id="devices"></tbody></table></div></section>
</main>
<script>
'use strict';
const byId=id=>document.getElementById(id);
let latestData=null;
function cell(row,text,cls){const td=row.insertCell();td.textContent=text;if(cls)td.className=cls;return td}
async function action(url,message){
 try{const r=await fetch(url,{method:'POST',headers:{'X-AntiStalker':'1'}});if(!r.ok)throw new Error('HTTP '+r.status);byId('status').textContent=message;await refresh()}
 catch(e){byId('status').textContent='Action failed: '+e.message}
}
function candidateState(c,isTracker){return !isTracker&&c.state==='FOLLOWING'?'CO-PRESENT':c.state}
const sorts={trackers:{key:'lastSeen',dir:-1},devices:{key:'lastSeen',dir:-1}};
const numericSortKeys=['rssi','sightings','locations','lastSeen','distanceM'];
function sortCompare(a,b,key,isTracker){
 if(key==='state'){
  const x=candidateState(a,isTracker),y=candidateState(b,isTracker);
  return x<y?-1:x>y?1:0
 }
 const x=a[key],y=b[key];
 if(typeof x==='string'||typeof y==='string'){return x<y?-1:x>y?1:0}
 return (x-y)||0
}
function render(target,rows,isTracker){
 const body=byId(target);body.textContent='';
 const query=byId(target+'Search').value.trim().toLowerCase();
 const visible=rows.filter(c=>[
  c.id,c.type,candidateState(c,isTracker),c.state,c.mac,c.addressType,c.rssi,c.peak,
  c.sightings,c.locations,c.lastSeen,c.owner,c.allowed?'allowed':'active',c.trustedName||''
 ].join(' ').toLowerCase().includes(query));
 const s=sorts[target];
 visible.sort((a,b)=>{const r=sortCompare(a,b,s.key,isTracker);return r?s.dir*r:(a.id<b.id?-1:a.id>b.id?1:0)});
 if(!rows.length){const row=body.insertRow();const td=cell(row,'No candidates observed yet.','muted');td.colSpan=10;return}
 if(!visible.length){const row=body.insertRow();const td=cell(row,'No matches for the current search.','muted');td.colSpan=10;return}
 for(const c of visible){
  const row=body.insertRow();cell(row,c.id);cell(row,c.type);
  const state=candidateState(c,isTracker);
  cell(row,state,'state '+state.toLowerCase().replace('co-present','following'));
  const macCell=cell(row,c.mac);
  if(c.trusted){const badge=document.createElement('span');badge.className='trustBadge';badge.textContent='TRUSTED';const rest=document.createElement('span');rest.textContent=(c.trustedName?c.trustedName+' · ':'')+c.mac;macCell.textContent='';macCell.append(badge,rest)}
  const dist=cell(row,'~'+c.distanceM+' m');if(c.distanceBand)dist.title=c.distanceBand;
  cell(row,c.rssi+' / '+c.peak+' dBm');cell(row,c.sightings);cell(row,c.locations);cell(row,c.lastSeen+' s');
  const button=document.createElement('button');button.className='secondary';button.textContent=c.allowed?'Unallow':'Allow';
  button.onclick=()=>action('/api/allow?id='+c.id+'&allowed='+(c.allowed?'0':'1'),c.allowed?'Alerts re-enabled.':'Device allowed.');cell(row,'').appendChild(button);
 }
}
function renderTables(){
 if(!latestData)return;
 render('trackers',latestData.trackers,true);render('devices',latestData.devices,false);updateSortIndicators();
}
function wireSortHeaders(){
 document.querySelectorAll('th[data-sort]').forEach(th=>{
  th.addEventListener('click',()=>{
   const tableId=th.closest('table').querySelector('tbody').id,s=sorts[tableId];
   if(s.key===th.dataset.sort){s.dir=-s.dir}else{s.key=th.dataset.sort;s.dir=numericSortKeys.includes(s.key)?-1:1}
   renderTables()
  })
 })
}
function updateSortIndicators(){
 document.querySelectorAll('th[data-sort]').forEach(th=>{
  const tableId=th.closest('table').querySelector('tbody').id,s=sorts[tableId];
  th.querySelector('.ind').textContent=th.dataset.sort===s.key?(s.dir>0?'▲':'▼'):''
 })
}
async function jsonAction(url,body,message){
 try{
  const r=await fetch(url,{method:'POST',headers:{'X-AntiStalker':'1'},body:JSON.stringify(body)});
  if(!r.ok){let detail='HTTP '+r.status;try{const j=await r.json();if(j.error)detail=j.error}catch(e){}throw new Error(detail)}
  byId('status').textContent=message;await refresh()
 }catch(e){byId('status').textContent='Action failed: '+e.message}
}
function renderTrust(d){
 const t=d.trust||{devices:[]};
 byId('trustedCount').textContent=t.devices.length;
 const box=byId('pairBox');box.textContent='';
 const btn=byId('pairBtn');
 if(t.pairing){
  btn.disabled=true;btn.textContent='Pairing window open…';
  const p=document.createElement('div');p.className='warning';
  p.innerHTML='<b>Pairing window open — '+t.expiresS+' s left.</b> On your phone: <b>Settings &gt; Bluetooth</b>, tap <b>'+t.name+'</b>, then enter this code.<div class="passkey">'+t.passkey+'</div>Detection pauses while the window is open.';
  const cancel=document.createElement('button');cancel.className='secondary';cancel.textContent='Cancel pairing';
  cancel.onclick=()=>action('/api/trust/cancel','Pairing window closed; passive scanning resumed.');
  p.appendChild(cancel);box.appendChild(p);
 }else{
  btn.disabled=false;btn.textContent='Pair a device';
 }
 const rows=byId('trustRows');rows.textContent='';
 if(!t.devices.length){const row=rows.insertRow();const td=cell(row,'No trusted devices paired yet.','muted');td.colSpan=4;return}
 for(const dev of t.devices){
  const row=rows.insertRow();cell(row,dev.name);cell(row,dev.identity+' · '+(dev.addressType?'random':'public'));cell(row,dev.resolved+'×');
  const actions=row.insertCell();
  const rename=document.createElement('button');rename.className='secondary';rename.textContent='Rename';
  rename.onclick=()=>{const n=prompt('Display name for this device:',dev.name);if(n&&n.trim())jsonAction('/api/trust/name',{address:dev.identity,name:n.trim()},'Trusted device renamed.')};
  const remove=document.createElement('button');remove.className='secondary';remove.textContent='Remove';
  remove.onclick=()=>{if(confirm('Remove '+dev.name+' from trusted devices? Its rotating addresses will no longer be recognized.'))jsonAction('/api/trust/remove',{address:dev.identity},'Trusted device removed.')};
  actions.append(rename,remove);
 }
}
function renderHome(d){
 const h=d.home||{valid:false};
 const box=byId('homeBox');box.textContent='';
 const p=document.createElement('p');p.className='muted';
 if(h.valid){
  p.innerHTML='Home: <b>'+h.latitude.toFixed(5)+', '+h.longitude.toFixed(5)+'</b>'+(h.source?' · set via '+h.source:'')+(h.phoneDistanceM!=null?' · phone is ~<b>'+Math.round(h.phoneDistanceM)+' m</b> from home':'')+'. Context only: alerts are never suppressed at home.';
 }else{
  p.textContent='Home is not set. Optional: adds a "phone is ~N m from home" note. Never suppresses alerts.';
 }
 box.appendChild(p);
 const row=document.createElement('div');row.className='homeRow';
 const gps=document.createElement('button');gps.className='secondary';gps.textContent='Use latest phone GPS fix';
 gps.disabled=!(d.phoneLocation&&d.phoneLocation.valid);
 gps.onclick=()=>{const f=d.phoneLocation;jsonAction('/api/home',{latitude:f.latitude,longitude:f.longitude,accuracy:f.accuracyM||0,source:'phone'},'Home set from the latest phone GPS fix.')};
 const manual=document.createElement('button');manual.className='secondary';manual.textContent='Enter coordinates…';
 manual.onclick=()=>{const s=prompt('Home coordinates (WGS84 decimal "lat, lon"):');if(!s)return;const m=s.split(/[,\s]+/).map(Number);if(m.length!==2||!isFinite(m[0])||!isFinite(m[1])||Math.abs(m[0])>90||Math.abs(m[1])>180){byId('status').textContent='Invalid coordinates.';return}jsonAction('/api/home',{latitude:m[0],longitude:m[1],source:'manual'},'Home position set.')};
 const clear=document.createElement('button');clear.className='secondary';clear.textContent='Remove';clear.disabled=!h.valid;
 clear.onclick=()=>{if(confirm('Remove the home position?'))action('/api/home/remove','Home position cleared.')};
 row.append(gps,manual,clear);box.appendChild(row);
}
async function startPairing(){await jsonAction('/api/trust/pair',{},'Pairing window open. Follow the instructions above.')}
async function refresh(){
 try{
  const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error('HTTP '+r.status);const d=await r.json();
  byId('ssid').textContent=d.ssid;byId('uptime').textContent=d.uptime+' s';byId('radio').textContent=d.scanning?'scanning':'stopped';
  byId('packets').textContent=d.received;byId('dropped').textContent=d.dropped;byId('epoch').textContent=d.locationEpoch;byId('alerts').textContent=d.alerts;
  if(typeof d.freeHeap==='number'){byId('heap').textContent=Math.round(d.freeHeap/1024)+' KiB free'}
  if(typeof d.temperatureC==='number'){byId('temp').textContent=(Math.round(d.temperatureC*10)/10)+' °C'}else{byId('temp').textContent='n/a'}
  if(d.battery&&d.battery.available&&d.battery.millivolts>0){byId('battery').textContent=(d.battery.millivolts/1000).toFixed(2)+' V · '+d.battery.percent+'%'}else{byId('battery').textContent='n/a'}
  if(d.phoneLocation&&d.phoneLocation.valid){byId('gps').textContent=(d.phoneLocation.accuracyM>0?'±'+Math.round(d.phoneLocation.accuracyM)+' m · ':'')+d.phoneLocation.ageS+' s old'}else{byId('gps').textContent='no fix'}
  latestData=d;renderTables();renderTrust(d);renderHome(d);
 }catch(e){byId('status').textContent='Refresh failed: '+e.message}
}
byId('moved').onclick=()=>{if(confirm('Confirm that you are now at a genuinely different physical location.'))action('/api/moved','New location confirmed. Keep scanning.')};
byId('trackersSearch').addEventListener('input',renderTables);byId('devicesSearch').addEventListener('input',renderTables);
function scheduleRefresh(){refresh().finally(()=>{setTimeout(scheduleRefresh,latestData&&latestData.trust&&latestData.trust.pairing?1000:3000)})}
wireSortHeaders();byId('pairBtn').onclick=startPairing;scheduleRefresh();
</script></body></html>)HTML";

bool parseId(const String& text, uint32_t& id) {
    if (text.length() == 0 || text.length() > 8) {
        return false;
    }
    for (size_t i = 0; i < text.length(); ++i) {
        if (!isxdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    id = static_cast<uint32_t>(strtoul(text.c_str(), nullptr, 16));
    return true;
}

const char* ownerState(const antistalker::Candidate& candidate) {
    if (!candidate.nearOwnerKnown) {
        return "unknown";
    }
    return candidate.nearOwner ? "near" : "separated";
}

// 16 chars from the hardware RNG, alphabet without the 0/O/1/l/I lookalikes,
// because someone WILL read this off a serial terminal at 2 AM.
// ~93 bits of entropy. WPA2 is the weak link here, not the password.
void generatePassword(char* destination, size_t size) {
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    const size_t length = 16;
    if (size <= length) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        destination[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    destination[length] = '\0';
}

} // namespace

WebPortal::WebPortal(antistalker::DetectionEngine& trackerEngine,
                     antistalker::DetectionEngine& deviceEngine, BleMonitor& monitor,
                     TrustManager& trust)
    : trackerEngine_(trackerEngine), deviceEngine_(deviceEngine), monitor_(monitor),
      trust_(trust), server_(80) {}

bool WebPortal::begin() {
    if (running_) {
        return true;
    }

    const uint64_t chipId = ESP.getEfuseMac();
    snprintf(ssid_, sizeof(ssid_), "AntiStalker-%04X", static_cast<unsigned>(chipId & 0xFFFFU));
    snprintf(hostname_, sizeof(hostname_), "antistalker-%04lx",
             static_cast<unsigned long>(chipId & 0xFFFFU));
    initBatterySensor();

    Preferences preferences;
    const bool storageReady = preferences.begin("antistalker", false);
    String storedPassword;
    if (storageReady) {
        storedPassword = preferences.getString("ap_password", "");
        homeEnabled_ = preferences.getBool("home_enabled", false);
        homeSsid_ = preferences.getString("home_ssid", "");
        homePassword_ = preferences.getString("home_password", "");
        if (preferences.getBool("home_valid", false)) {
            home_.valid = true;
            home_.latitude = preferences.getDouble("home_lat", 0.0);
            home_.longitude = preferences.getDouble("home_lon", 0.0);
            home_.accuracyM = static_cast<float>(preferences.getDouble("home_accuracy", 0.0));
            strlcpy(home_.source, preferences.getString("home_source", "manual").c_str(),
                    sizeof(home_.source));
        }
    }
    if (storedPassword.length() >= 12) {
        strncpy(password_, storedPassword.c_str(), sizeof(password_) - 1);
        password_[sizeof(password_) - 1] = '\0';
    } else {
        generatePassword(password_, sizeof(password_));
        if (storageReady) {
            preferences.putString("ap_password", password_);
        }
    }
    if (storageReady) {
        preferences.end();
    }
    if (password_[0] == '\0') {
        return false;
    }

    // We manage credentials in our own NVS namespace, not in the Wi-Fi
    // driver's flash, and we decide when to reconnect (see handle()).
    WiFi.persistent(false);
    WiFi.setAutoReconnect(false); // handle() retries on a controlled 30-second cadence.
    if (!startAccessPoint()) {
        WiFi.mode(WIFI_OFF);
        return false;
    }

    configureRoutes();
    server_.begin();
    startMdns();
    running_ = true;
    homeReconnectPending_ = false;
    homeWasConnected_ = false;
    if (homeEnabled_ && homeSsid_.length() > 0) {
        connectHome();
    }
    return true;
}

void WebPortal::stop() {
    if (!running_) {
        return;
    }
    server_.stop();
    MDNS.end();
    homeReconnectPending_ = false;
    homeWasConnected_ = false;
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apRunning_ = false;
    running_ = false;
}

void WebPortal::startMdns() {
    if (hostname_[0] == '\0' || !MDNS.begin(hostname_)) {
        Serial.println("[WIFI] mDNS registration failed; the app will fall back to IP discovery.");
        return;
    }
    // antistalker-XXXX.local: so the app finds the board on home Wi-Fi by
    // name, and nobody has to go dig the DHCP lease out of the router UI.
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[WIFI] mDNS name: http://%s.local\n", hostname_);
}

// The Wi-Fi state machine, called every loop(). Rules:
//   - home connected      -> AP off, done
//   - home lost           -> AP back on (within 5 s), retry home every 30 s
//   - settings just saved -> wait 1 s so the HTTP reply actually leaves,
//                            THEN rip the network out from under the client
void WebPortal::handle() {
    if (!running_) {
        return;
    }
    server_.handleClient();

    const uint32_t now = millis();
    if (homeReconnectPending_) {
        // Give the HTTP response time to leave before changing either interface.
        if (static_cast<int32_t>(now - homeReconnectAtMs_) < 0) {
            return;
        }
        homeReconnectPending_ = false;
        if (homeEnabled_ && homeSsid_.length() > 0) {
            connectHome();
        } else {
            WiFi.disconnect();
            homeWasConnected_ = false;
            startAccessPoint();
        }
    }

    const bool homeConfigured = homeEnabled_ && homeSsid_.length() > 0;
    const bool homeConnected = homeConfigured && WiFi.status() == WL_CONNECTED;
    if (homeConnected) {
        if (!homeWasConnected_) {
            homeWasConnected_ = true;
            Serial.printf("[WIFI] Connected to home network '%s', address=%s; fallback hotspot stopped.\n",
                          homeSsid_.c_str(), WiFi.localIP().toString().c_str());
        }
        stopAccessPoint();
        return;
    }

    if (homeWasConnected_) {
        homeWasConnected_ = false;
        Serial.println("[WIFI] Home network lost; starting fallback hotspot.");
    }
    if (!apRunning_ && now - lastApAttemptMs_ >= kApRetryIntervalMs) {
        startAccessPoint();
    }
    if (homeConfigured && now - lastHomeAttemptMs_ >= kHomeRetryIntervalMs) {
        connectHome();
    }
}

void WebPortal::printCredentials() const {
    Serial.printf("Wi-Fi fallback AP: ssid=%s password=%s state=%s\n", ssid_, password_,
                  apRunning_ ? "on" : "off");
    if (apRunning_) {
        Serial.printf("Wi-Fi AP URL: http://%s clients=%u\n", WiFi.softAPIP().toString().c_str(),
                      WiFi.softAPgetStationNum());
    }
    if (hostname_[0] != '\0') {
        Serial.printf("mDNS URL (on home Wi-Fi): http://%s.local\n", hostname_);
    }
    if (!homeEnabled_) {
        Serial.println("Home Wi-Fi: disabled");
    } else if (homeSsid_.length() == 0) {
        Serial.println("Home Wi-Fi: enabled but no network is configured");
    } else if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Home Wi-Fi: connected ssid=%s url=http://%s\n", homeSsid_.c_str(),
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("Home Wi-Fi: unavailable ssid=%s; retrying automatically\n", homeSsid_.c_str());
    }
}

bool WebPortal::startAccessPoint() {
    if (apRunning_) {
        return true;
    }
    lastApAttemptMs_ = millis();
    WiFi.mode(homeEnabled_ && homeSsid_.length() > 0 ? WIFI_AP_STA : WIFI_AP);
    WiFi.setSleep(true);
    // 5 dBm: enough for "phone in the same room", low enough to keep current
    // spikes down and not advertise ourselves to the whole street.
    WiFi.setTxPower(WIFI_POWER_5dBm);
    const IPAddress address(192, 168, 4, 1);
    const IPAddress gateway(192, 168, 4, 1);
    const IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(address, gateway, subnet);
    apRunning_ = WiFi.softAP(ssid_, password_, 6, false, 2); // channel 6, visible, max 2 clients
    if (!apRunning_) {
        Serial.println("[WIFI] Failed to start fallback hotspot; will retry.");
    }
    return apRunning_;
}

void WebPortal::stopAccessPoint() {
    if (!apRunning_) {
        return;
    }
    WiFi.softAPdisconnect(true);
    apRunning_ = false;
}

void WebPortal::connectHome() {
    if (!homeEnabled_ || homeSsid_.length() == 0) {
        return;
    }
    if (!apRunning_) {
        startAccessPoint();
    }
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();
    WiFi.setAutoReconnect(false); // handle() retries on a controlled 30-second cadence.
    if (homePassword_.length() == 0) {
        WiFi.begin(homeSsid_.c_str());
    } else {
        WiFi.begin(homeSsid_.c_str(), homePassword_.c_str());
    }
    lastHomeAttemptMs_ = millis();
    homeWasConnected_ = false;
    Serial.printf("[WIFI] Trying home network '%s'; fallback hotspot remains on until connected.\n",
                  homeSsid_.c_str());
}

void WebPortal::configureRoutes() {
    if (routesConfigured_) {
        return;
    }
    static const char* collectedHeaders[] = {"X-AntiStalker"};
    server_.collectHeaders(collectedHeaders, 1);
    server_.on("/", HTTP_GET, [this]() { sendIndex(); });
    server_.on("/api/status", HTTP_GET, [this]() { sendStatus(); });
    server_.on("/api/moved", HTTP_POST, [this]() { handleMoved(); });
    server_.on("/api/allow", HTTP_POST, [this]() { handleAllow(); });
    server_.on("/api/wifi", HTTP_POST, [this]() { handleWifi(); });
    server_.on("/api/location", HTTP_POST, [this]() { handleLocation(); });
    server_.on("/api/trust/pair", HTTP_POST, [this]() { handleTrustPair(); });
    server_.on("/api/trust/cancel", HTTP_POST, [this]() { handleTrustCancel(); });
    server_.on("/api/trust/name", HTTP_POST, [this]() { handleTrustRename(); });
    server_.on("/api/trust/remove", HTTP_POST, [this]() { handleTrustRemove(); });
    server_.on("/api/home", HTTP_POST, [this]() { handleHome(); });
    server_.on("/api/home/remove", HTTP_POST, [this]() { handleHomeRemove(); });
    server_.onNotFound([this]() { sendJson("{\"error\":\"not found\"}", 404); });
    routesConfigured_ = true;
}

// Strict-ish CSP: everything inline, nothing external, no framing. The page
// can only talk to the board itself ('connect-src self').
void WebPortal::sendIndex() {
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("X-Content-Type-Options", "nosniff");
    server_.sendHeader("Content-Security-Policy",
                       "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                       "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    server_.send_P(200, "text/html; charset=utf-8", kIndexPage);
}

// The big one: one JSON blob with system health, network state, GPS, home,
// trust and both candidate tables. The app polls this every few seconds.
// Bump "version" when the shape changes; the app checks optional fields.
void WebPortal::sendStatus() {
    JsonDocument doc;
    const uint32_t now = millis();
    doc["version"] = "0.5.0";
    doc["ssid"] = ssid_;
    if (hostname_[0] != '\0') {
        doc["mdns"] = String(hostname_) + ".local";
    }
    doc["uptime"] = now / 1000UL;
    doc["scanning"] = monitor_.scanning();
    doc["received"] = monitor_.receivedAdvertisements();
    doc["dropped"] = monitor_.droppedAdvertisements();
    doc["trackerPackets"] = monitor_.matchedAdvertisements();
    doc["devicePackets"] = monitor_.personalAdvertisements();
    doc["devicesEnabled"] = monitor_.personalDevicesEnabled();
    doc["locationEpoch"] = trackerEngine_.locationEpoch();
    doc["alerts"] = trackerEngine_.activeAlertCount() + deviceEngine_.activeAlertCount();
    doc["wifiClients"] = apRunning_ ? WiFi.softAPgetStationNum() : 0;
    const bool homeConnected = homeEnabled_ && homeSsid_.length() > 0 && WiFi.status() == WL_CONNECTED;
    JsonObject homeWifi = doc["homeWifi"].to<JsonObject>();
    homeWifi["enabled"] = homeEnabled_;
    homeWifi["configured"] = homeSsid_.length() > 0;
    homeWifi["connected"] = homeConnected;
    homeWifi["ssid"] = homeSsid_;
    homeWifi["ip"] = homeConnected ? WiFi.localIP().toString() : String();
    appendSystem(doc);
    appendPhoneLocation(doc);
    appendHome(doc);
    appendTrust(doc);
    appendCandidates(doc["trackers"].to<JsonArray>(), trackerEngine_, now);
    appendCandidates(doc["devices"].to<JsonArray>(), deviceEngine_, now);

    String response;
    serializeJson(doc, response);
    if (doc.overflowed() || response.length() == 0) {
        sendJson("{\"error\":\"response allocation failed\"}", 500);
        return;
    }
    sendJson(response);
}

void WebPortal::appendSystem(JsonDocument& doc) {
    doc["chipModel"] = ESP.getChipModel();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["totalHeap"] = ESP.getHeapSize();
    doc["freePsram"] = ESP.getFreePsram();
    doc["totalPsram"] = ESP.getPsramSize();
    doc["sketchSize"] = ESP.getSketchSize();
    doc["flashFree"] = ESP.getFreeSketchSpace();
    doc["maxCandidates"] = static_cast<uint32_t>(antistalker::kMaxCandidates);

    const float temperature = temperatureRead();
    if (!isnan(temperature)) {
        doc["temperatureC"] = roundf(temperature * 10.0f) / 10.0f;
    } else {
        doc["temperatureC"] = nullptr; // The sensor said NaN. We respect its silence.
    }

    JsonObject battery = doc["battery"].to<JsonObject>();
    if (kBatteryPin >= 0) {
        const uint32_t millivolts = readBatteryMillivolts();
        battery["available"] = true;
        battery["millivolts"] = millivolts;
        battery["percent"] = batteryPercentEstimate(millivolts);
    } else {
        battery["available"] = false;
        battery["millivolts"] = nullptr;
        battery["percent"] = nullptr;
    }
}

void WebPortal::appendPhoneLocation(JsonDocument& doc) {
    JsonObject location = doc["phoneLocation"].to<JsonObject>();
    location["valid"] = phoneLocation_.valid;
    location["latitude"] = phoneLocation_.valid ? phoneLocation_.latitude : 0.0;
    location["longitude"] = phoneLocation_.valid ? phoneLocation_.longitude : 0.0;
    location["accuracyM"] = phoneLocation_.valid ? phoneLocation_.accuracyM : 0.0f;
    location["ageS"] = phoneLocation_.valid
                           ? (millis() - phoneLocation_.receivedMs) / 1000UL
                           : 0UL;
    location["autoEpochDistanceM"] = kAutoEpochDistanceM;
}

void WebPortal::handleMoved() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    trackerEngine_.markLocationChanged();
    deviceEngine_.markLocationChanged();
    // Re-anchor auto-epochs to the latest phone fix (or the next one). With
    // a stale anchor, the very next GPS fix could look like "moved 2 km!"
    // and burn a second epoch right after the manual one.
    adoptLocationAnchor();
    Serial.printf("[WEB] Location epoch %lu confirmed.\n",
                  static_cast<unsigned long>(trackerEngine_.locationEpoch()));
    sendJson("{\"ok\":true}");
}

void WebPortal::handleLocation() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    if (updatePhoneLocationFromRequest()) {
        sendJson("{\"ok\":true}");
    }
}

void WebPortal::adoptLocationAnchor() {
    epochAnchor_ = phoneLocation_; // Copies valid=false when no fix exists yet.
}

bool WebPortal::updatePhoneLocationFromRequest() {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, server_.arg("plain"));
    if (error || !doc.is<JsonObject>()) {
        sendJson("{\"error\":\"invalid JSON body\"}", 400);
        return false;
    }

    const JsonObject request = doc.as<JsonObject>();
    if (!request["latitude"].is<double>() || !request["longitude"].is<double>()) {
        sendJson("{\"error\":\"latitude and longitude must be numbers\"}", 400);
        return false;
    }
    const double latitude = request["latitude"].as<double>();
    const double longitude = request["longitude"].as<double>();
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        sendJson("{\"error\":\"latitude or longitude out of range\"}", 400);
        return false;
    }

    float accuracyM = 0.0f;
    if (!request["accuracy"].isNull()) {
        if (!request["accuracy"].is<double>() || request["accuracy"].as<double>() < 0.0 ||
            request["accuracy"].as<double>() > 100000.0) {
            sendJson("{\"error\":\"accuracy must be meters (0-100000)\"}", 400);
            return false;
        }
        accuracyM = request["accuracy"].as<float>();
    }

    PhoneLocation fix;
    fix.valid = true;
    fix.latitude = latitude;
    fix.longitude = longitude;
    fix.accuracyM = accuracyM;
    fix.receivedMs = millis();
    phoneLocation_ = fix;
    Serial.printf("[GPS] Phone fix: lat=%.5f lon=%.5f accuracy=%ldm\n", latitude, longitude,
                  lroundf(accuracyM));

    if (!epochAnchor_.valid) {
        epochAnchor_ = fix;
        return true;
    }

    // Automatic epoch: you actually went somewhere, according to your phone's
    // own GPS, with a fix good enough to believe. Fixes with accuracy 0
    // ("I don't know") never qualify.
    if (fix.accuracyM > 0.0f && fix.accuracyM <= kAutoEpochMaxAccuracyM &&
        distanceMeters(epochAnchor_.latitude, epochAnchor_.longitude, fix.latitude,
                        fix.longitude) >= kAutoEpochDistanceM) {
        trackerEngine_.markLocationChanged();
        deviceEngine_.markLocationChanged();
        epochAnchor_ = fix;
        Serial.printf("[GPS] Movement over %.0fm confirmed from phone GPS; location epoch %lu "
                      "started automatically.\n",
                      static_cast<double>(kAutoEpochDistanceM),
                      static_cast<unsigned long>(trackerEngine_.locationEpoch()));
    }
    return true;
}

void WebPortal::handleAllow() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    if (setAllowedFromRequest()) {
        sendJson("{\"ok\":true}");
    }
}

void WebPortal::handleWifi() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    if (updateHomeWifiFromRequest()) {
        sendJson("{\"ok\":true}");
    }
}

bool WebPortal::updateHomeWifiFromRequest() {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, server_.arg("plain"));
    if (error || !doc.is<JsonObject>()) {
        sendJson("{\"error\":\"invalid JSON body\"}", 400);
        return false;
    }

    const JsonObject request = doc.as<JsonObject>();
    if (!request["enabled"].is<bool>()) {
        sendJson("{\"error\":\"enabled must be a boolean\"}", 400);
        return false;
    }
    const bool enabled = request["enabled"].as<bool>();
    String nextSsid = homeSsid_;
    String nextPassword = homePassword_;
    if (!request["ssid"].isNull() || !request["password"].isNull()) {
        if (!request["ssid"].is<const char*>() || !request["password"].is<const char*>()) {
            sendJson("{\"error\":\"ssid and password must be strings\"}", 400);
            return false;
        }
        nextSsid = request["ssid"].as<const char*>();
        nextPassword = request["password"].as<const char*>();
    }

    if (nextSsid.length() > 32 || (enabled && nextSsid.length() == 0)) {
        sendJson("{\"error\":\"enter a home Wi-Fi name of 1-32 bytes before enabling\"}", 400);
        return false;
    }
    const size_t passwordLength = nextPassword.length();
    if (passwordLength > 0 && (passwordLength < 8 || passwordLength > 63)) {
        sendJson("{\"error\":\"home Wi-Fi password must be empty or 8-63 bytes\"}", 400);
        return false;
    }

    Preferences preferences;
    if (!preferences.begin("antistalker", false)) {
        sendJson("{\"error\":\"settings storage unavailable\"}", 500);
        return false;
    }
    preferences.putBool("home_enabled", enabled);
    preferences.putString("home_ssid", nextSsid);
    preferences.putString("home_password", nextPassword);
    preferences.end();

    homeEnabled_ = enabled;
    homeSsid_ = nextSsid;
    homePassword_ = nextPassword;
    homeReconnectPending_ = true;
    homeReconnectAtMs_ = millis() + kHomeReconnectDelayMs;
    Serial.printf("[WEB] Home Wi-Fi %s for ssid='%s'; applying network change.\n",
                  enabled ? "enabled" : "disabled", homeSsid_.c_str());
    return true;
}

bool WebPortal::setAllowedFromRequest() {
    uint32_t id;
    if (!parseId(server_.arg("id"), id)) {
        sendJson("{\"error\":\"invalid id\"}", 400);
        return false;
    }
    const bool allowed = server_.arg("allowed") != "0";
    const bool trackerFound = trackerEngine_.allow(id, allowed);
    const bool deviceFound = deviceEngine_.allow(id, allowed);
    if (!trackerFound && !deviceFound) {
        sendJson("{\"error\":\"candidate not found\"}", 404);
        return false;
    }
    return true;
}

// NOT authentication (there is none, the network password is the gate).
// It is a CSRF barrier: browsers cannot attach a custom header cross-origin
// without a CORS preflight, and this server answers no preflights.
bool WebPortal::actionAuthorized() {
    return server_.hasHeader("X-AntiStalker") && server_.header("X-AntiStalker") == "1";
}

void WebPortal::sendJson(const String& json, int code) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("X-Content-Type-Options", "nosniff");
    server_.send(code, "application/json", json);
}

void WebPortal::appendHome(JsonDocument& doc) {
    JsonObject home = doc["home"].to<JsonObject>();
    home["valid"] = home_.valid;
    if (!home_.valid) {
        home["latitude"] = nullptr;
        home["longitude"] = nullptr;
        home["accuracyM"] = nullptr;
        home["ageS"] = nullptr;
        home["source"] = "";
    } else {
        home["latitude"] = home_.latitude;
        home["longitude"] = home_.longitude;
        home["accuracyM"] = home_.accuracyM;
        home["ageS"] = (millis() - home_.setMs) / 1000UL;
        home["source"] = home_.source;
    }
    // Context only ("phone is ~N m from home"), never an alert filter.
    // Same reason as in WebPortal.h: the tag in your bag is home too.
    if (home_.valid && phoneLocation_.valid) {
        home["phoneDistanceM"] = distanceMeters(home_.latitude, home_.longitude,
                                                phoneLocation_.latitude, phoneLocation_.longitude);
    } else {
        home["phoneDistanceM"] = nullptr;
    }
}

void WebPortal::appendTrust(JsonDocument& doc) {
    JsonObject trust = doc["trust"].to<JsonObject>();
    trust["pairing"] = trust_.pairingActive();
    if (trust_.pairingActive()) {
        trust["name"] = trust_.pairingName();
        char passkey[8];
        snprintf(passkey, sizeof(passkey), "%06lu", static_cast<unsigned long>(trust_.passkey()));
        trust["passkey"] = passkey; // String, so "004217" stays "004217" and not 4217.
        trust["expiresS"] = trust_.pairingRemainingS();
    }
    JsonArray devices = trust["devices"].to<JsonArray>();
    for (size_t i = 0; i < antistalker::TrustResolver::kMaxIdentities; ++i) {
        antistalker::TrustIdentity identity;
        if (!trust_.deviceAt(i, identity)) {
            continue;
        }
        JsonObject item = devices.add<JsonObject>();
        item["name"] = identity.name;
        char address[18];
        antistalker::TrustResolver::formatAddress(identity.address, address, sizeof(address));
        item["identity"] = address;
        item["addressType"] = identity.addressType;
        item["resolved"] = identity.resolvedCount;
    }
}

void WebPortal::appendCandidates(JsonArray array, const antistalker::DetectionEngine& engine,
                                 uint32_t nowMs) {
    for (size_t i = 0; i < antistalker::kMaxCandidates; ++i) {
        const antistalker::Candidate* candidate = engine.candidateAt(i);
        if (candidate == nullptr) {
            continue;
        }

        JsonObject item = array.add<JsonObject>();
        char id[9];
        snprintf(id, sizeof(id), "%08lX", static_cast<unsigned long>(candidate->id));
        item["id"] = id;
        item["type"] = antistalker::trackerFamilyName(candidate->family);
        item["state"] = antistalker::alertLevelName(candidate->level);
        item["mac"] = candidate->address;
        item["addressType"] = candidate->addressType;
        item["rssi"] = candidate->filteredRssi;
        item["peak"] = candidate->strongestRssi;
        item["sightings"] = candidate->sightings;
        item["minutes"] = candidate->distinctMinutes;
        item["locations"] = candidate->locations;
        item["owner"] = ownerState(*candidate);
        item["lastSeen"] = (nowMs - candidate->lastSeenMs) / 1000UL;
        item["allowed"] = candidate->allowed;
        item["trusted"] = candidate->trusted;
        if (candidate->trusted) {
            uint8_t identityAddress[6];
            if (antistalker::TrustResolver::parseAddress(candidate->address, identityAddress)) {
                const antistalker::TrustIdentity* identity =
                    trust_.resolver().findIdentity(identityAddress, candidate->addressType);
                if (identity != nullptr && identity->name[0] != '\0') {
                    item["trustedName"] = identity->name;
                }
            }
        }
        // Rough RSSI-based distance. Walls, pockets, bodies and antenna
        // orientation easily make it 2x off in either direction, so the
        // "about 1-4 m" band ships next to the number, always. RSSI is not a
        // tape measure, no matter how much we want it to be.
        item["distanceM"] = antistalker::estimateDistanceM(candidate->filteredRssi);
        item["distanceBand"] = antistalker::distanceBand(candidate->filteredRssi);
        if (candidate->family == antistalker::TrackerFamily::Dult) {
            item["network"] = candidate->networkId;
        }
    }
}

void WebPortal::handleTrustPair() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    uint32_t seconds = 0;
    if (!server_.arg("plain").isEmpty()) {
        JsonDocument doc;
        if (deserializeJson(doc, server_.arg("plain")) || !doc.is<JsonObject>()) {
            sendJson("{\"error\":\"invalid JSON body\"}", 400);
            return;
        }
        if (!doc["seconds"].isNull()) {
            if (!doc["seconds"].is<uint32_t>() || doc["seconds"].as<uint32_t>() == 0) {
                sendJson("{\"error\":\"seconds must be a positive number\"}", 400);
                return;
            }
            seconds = doc["seconds"].as<uint32_t>();
        }
    }
    uint32_t passkey = 0;
    if (!trust_.startPairing(seconds, passkey)) {
        sendJson("{\"error\":\"could not start pairing; try again\"}", 500);
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    char passkeyText[8];
    snprintf(passkeyText, sizeof(passkeyText), "%06lu", static_cast<unsigned long>(passkey));
    doc["passkey"] = passkeyText;
    doc["name"] = trust_.pairingName();
    doc["expiresS"] = trust_.pairingRemainingS();
    String response;
    serializeJson(doc, response);
    sendJson(response);
}

void WebPortal::handleTrustCancel() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    trust_.cancelPairing();
    sendJson("{\"ok\":true}");
}

void WebPortal::handleTrustRename() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server_.arg("plain")) || !doc.is<JsonObject>()) {
        sendJson("{\"error\":\"invalid JSON body\"}", 400);
        return;
    }
    const JsonObject request = doc.as<JsonObject>();
    if (!request["address"].is<const char*>() || !request["name"].is<const char*>()) {
        sendJson("{\"error\":\"address and name must be strings\"}", 400);
        return;
    }
    const String address = request["address"].as<const char*>();
    const String name = request["name"].as<const char*>();
    if (name.length() == 0 || name.length() > 24) {
        sendJson("{\"error\":\"name must be 1-24 bytes\"}", 400);
        return;
    }
    if (!trust_.renameDevice(address.c_str(), name.c_str())) {
        sendJson("{\"error\":\"trusted device not found\"}", 404);
        return;
    }
    Serial.printf("[WEB] Trusted device %s renamed to '%s'.\n", address.c_str(), name.c_str());
    sendJson("{\"ok\":true}");
}

void WebPortal::handleTrustRemove() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, server_.arg("plain")) || !doc.is<JsonObject>()) {
        sendJson("{\"error\":\"invalid JSON body\"}", 400);
        return;
    }
    const JsonObject request = doc.as<JsonObject>();
    if (!request["address"].is<const char*>()) {
        sendJson("{\"error\":\"address must be a string\"}", 400);
        return;
    }
    const String address = request["address"].as<const char*>();
    if (!trust_.removeDevice(address.c_str())) {
        sendJson("{\"error\":\"trusted device not found\"}", 404);
        return;
    }
    sendJson("{\"ok\":true}");
}

void WebPortal::handleHome() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    if (updateHomeFromRequest()) {
        sendJson("{\"ok\":true}");
    }
}

void WebPortal::handleHomeRemove() {
    if (!actionAuthorized()) {
        sendJson("{\"error\":\"forbidden\"}", 403);
        return;
    }
    HomeLocation next; // valid=false, i.e. "forget it"
    if (!persistHome(next)) {
        sendJson("{\"error\":\"settings storage unavailable\"}", 500);
        return;
    }
    home_ = next;
    Serial.println("[WEB] Home position cleared.");
    sendJson("{\"ok\":true}");
}

bool WebPortal::persistHome(const HomeLocation& next) {
    Preferences preferences;
    if (!preferences.begin("antistalker", false)) {
        return false;
    }
    preferences.putBool("home_valid", next.valid);
    if (next.valid) {
        preferences.putDouble("home_lat", next.latitude);
        preferences.putDouble("home_lon", next.longitude);
        preferences.putDouble("home_accuracy", next.accuracyM);
        preferences.putString("home_source", next.source);
    }
    preferences.end();
    return true;
}

bool WebPortal::updateHomeFromRequest() {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, server_.arg("plain"));
    if (error || !doc.is<JsonObject>()) {
        sendJson("{\"error\":\"invalid JSON body\"}", 400);
        return false;
    }

    const JsonObject request = doc.as<JsonObject>();
    if (!request["latitude"].is<double>() || !request["longitude"].is<double>()) {
        sendJson("{\"error\":\"latitude and longitude must be numbers\"}", 400);
        return false;
    }
    const double latitude = request["latitude"].as<double>();
    const double longitude = request["longitude"].as<double>();
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        sendJson("{\"error\":\"latitude or longitude out of range\"}", 400);
        return false;
    }

    float accuracyM = 0.0f;
    if (!request["accuracy"].isNull()) {
        if (!request["accuracy"].is<double>() || request["accuracy"].as<double>() < 0.0 ||
            request["accuracy"].as<double>() > 100000.0) {
            sendJson("{\"error\":\"accuracy must be meters (0-100000)\"}", 400);
            return false;
        }
        accuracyM = request["accuracy"].as<float>();
    }

    const char* source = "manual";
    if (!request["source"].isNull()) {
        if (!request["source"].is<const char*>()) {
            sendJson("{\"error\":\"source must be a string\"}", 400);
            return false;
        }
        source = request["source"].as<const char*>();
        if (strcmp(source, "phone") != 0 && strcmp(source, "map") != 0 &&
            strcmp(source, "manual") != 0) {
            sendJson("{\"error\":\"source must be phone, map or manual\"}", 400);
            return false;
        }
    }

    HomeLocation next;
    next.valid = true;
    next.latitude = latitude;
    next.longitude = longitude;
    next.accuracyM = accuracyM;
    next.setMs = millis();
    strlcpy(next.source, source, sizeof(next.source));
    if (!persistHome(next)) {
        sendJson("{\"error\":\"settings storage unavailable\"}", 500);
        return false;
    }
    home_ = next;
    Serial.printf("[WEB] Home position set: lat=%.5f lon=%.5f source=%s\n", latitude, longitude,
                  home_.source);
    return true;
}
