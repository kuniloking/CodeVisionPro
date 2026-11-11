/*
  ESP8266 - Premium Smart Home App (RemoteXY + SinricPro)
  ────────────────────────────────────────────────────
  * ULTIMATE STABLE CRASH-PROOF VERSION *
  ✅ FIX 1: IP address ab sahi dikhta hai (0.0.0.0 issue fixed)
  ✅ FIX 2: Relay toggle AJAX based - NO page reload
  ✅ FIX 3: gReconnecting variable properly declared
  ✅ FIX 4: Light/Fan no longer turns ON/OFF by itself (SinricPro & RemoteXY sync fixed)
  ✅ FIX 5 (NEW): Flash write debouncing - No more crashes on rapid relay clicks!
  ✅ FIX 6 (NEW): WiFi.persistent(false) - Prevents flash corruption on WiFi drops.
  ✅ FIX 7 (NEW): Zero-allocation char arrays for JSON - Stops RAM fragmentation crashes!
  ✅ FIX 8 (NEW): Native SDK WiFi auto-(re)connect DISABLED. Reconnection is now
     owned by a custom ASYNC connection manager (2s link poll, 15s join timeout,
     5-60s exponential backoff, zero delay() calls). The hidden SDK retry loop
     used to fight our own retries, churn lwIP sockets and crash the heap over days.
  ✅ FIX 9 (NEW): Ghost-trigger-proof relay GPIOs - internal pull-ups engaged
     first, output latch pre-written HIGH (relay OFF) before the pin ever
     switches to OUTPUT, persisted states applied explicitly, plus a 1s guard
     re-assert so a transient glitch can never latch a relay on.
  ✅ FIX 10 (NEW): RemoteXY cloud objects are created ONCE per boot - the old
     code re-allocated them on every WiFi drop (slow heap leak!). Credential and
     token changes now apply via a clean scheduled restart instead.
  ✅ FIX 11 (NEW): UI status banner - live local IP when online (green), an
     explicit "WiFi Disconnected" error when offline (red), device-unreachable
     detection in the browser, and a free-heap monitor on the Diagnostics page.
  ✅ FIX 12 (NEW): SoftAP hotspot visibility - SoftAP is started ONCE and kept
     alive. Reconnect churn no longer restarts softAP()/disconnects the AP, so
     "ESP_SmartHome" stays visible in the phone WiFi list. Explicit channel 1,
     max 4 clients, SoftAP settle delay before first STA join, and a beacon
     health-check that only restarts SoftAP if it actually died.
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>

#undef UNUSED  

#include <RemoteXY.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>

// ── Forward Declarations ──────────
void startCloud();
void startSinric();
void applyRelays();
void setupHotspot(bool forceRestart = false);
void ensureHotspotAlive();
void saveRelayStates();
void loadRelayStates();
void processEEPROMSave();
void addLog(String line);
void initRelayGPIO();
void wifiManagerLoop();
void wcmStartAttempt();
void scheduleRestart(unsigned long delayMs);

// ── RemoteXY Cloud Settings ───────────────────────────────────────────────────
#define REMOTEXY_CLOUD_SERVER  "cloud.remotexy.com"
#define REMOTEXY_CLOUD_PORT    6376
#define REMOTEXY_CLOUD_TOKEN   "38ecb531853efad8dde695608b62604d"

#pragma pack(push, 1)
uint8_t const PROGMEM RemoteXY_CONF_PROGMEM[] =
  { 254,2,0,0,0,0,0,1,0,1,1,0,53,0,21,0,0,0,69,83,
  80,32,48,49,0,31,1,106,200,1,1,2,0,2,13,11,44,22,0,2,
  26,31,31,79,78,0,79,70,70,0,2,14,46,44,22,0,2,26,31,31,
  79,78,0,79,70,70,0 };
struct {
  uint8_t switch_01;
  uint8_t switch_02;
  uint8_t connect_flag;
} RemoteXY;
#pragma pack(pop)

// ── Default Hotspot Credentials ───────────────────────────────────────────────
String apSSID = "ESP_SmartHome";
String apPass = "12345678";

// ── Relay Pins (ESP-01) ───────────────────────────────────────────────────────
#define PIN_SWITCH_01  0   // GPIO0 - Fan   (active-LOW)
#define PIN_SWITCH_02  2   // GPIO2 - Light (active-LOW)

// ── EEPROM Layout ─────────────────────────────────────────────────────────────
#define EEPROM_SIZE    512
#define ADDR_SSID      0    // 32 bytes
#define ADDR_PASS      32   // 32 bytes
#define ADDR_TOKEN     64   // 48 bytes
#define ADDR_VALID     112  // 1 byte    (magic)
#define ADDR_SP_KEY    113  // 36 bytes
#define ADDR_SP_SEC    149  // 128 bytes
#define ADDR_SP_FAN    277  // 26 bytes
#define ADDR_SP_LIGHT  303  // 26 bytes
#define ADDR_AUTO_CONN 329  // 1 byte (Auto Reconnect Toggle)
#define ADDR_AP_SSID   330  // 32 bytes (Hotspot Name)
#define ADDR_AP_PASS   362  // 32 bytes (Hotspot Pass)
#define ADDR_FAN_STATE   394  // 1 byte
#define ADDR_LIGHT_STATE 395  // 1 byte
#define EEPROM_MAGIC   0xA5

// ── Runtime State ─────────────────────────────────────────────────────────────
ESP8266WebServer server(80);

String cfgSSID    = "";
String cfgPass    = "";
String cfgToken   = REMOTEXY_CLOUD_TOKEN;
String spKey      = "";   
String spSecret   = "";   
String spFanId    = "";   
String spLightId  = "";   
bool cfgAutoConn  = true; 

bool cloudStarted  = false;
bool sinricStarted = false;
bool sinricOnline  = false;

bool fanState   = false;
bool lightState = false;

int  gRetryCount   = 0;
bool gReconnecting = false;
String lastResetReason = "";

// RemoteXY reconnection state tracking
uint8_t       rxyLastFlag     = 255;
unsigned long rxyConnectTime  = 0;
bool          rxySynced       = false;
const unsigned long RXY_SYNC_DELAY = 3000;

// [FIX 5] EEPROM Write Debouncing
bool relayStatesDirty = false;
unsigned long lastRelayChangeTime = 0;

// [FIX 8] Custom async WiFi connection manager - types, state and timing.
enum WifiConnState : uint8_t { WCS_IDLE, WCS_CONNECTING, WCS_ONLINE, WCS_RETRY_WAIT };

WifiConnState wcState     = WCS_IDLE;
unsigned long wcStateTime = 0;
unsigned long wcNextPoll  = 0;
bool          wifiIsUp    = false;

const unsigned long WCM_POLL_MS    = 2000;
const unsigned long WCM_TIMEOUT_MS = 15000;
const unsigned long WCM_RETRY_BASE = 5000;
const unsigned long WCM_RETRY_MAX  = 60000;

// [FIX 12] SoftAP lifecycle - start once, keep beaconing, restart only if dead
bool          apStarted      = false;
unsigned long apSettleUntil  = 0;      // STA join waits until SoftAP beacons settle
const unsigned long AP_SETTLE_MS = 1000;
const int     AP_CHANNEL     = 1;      // phones find ch 1/6/11 most reliably
bool          scanWasRunning = false;  // re-assert SoftAP after WiFi scan ends

// ── [FIX 10] Scheduled Restart (non-blocking) ────────────────────────────────
bool          restartPending = false;
unsigned long restartAtMs    = 0;

void scheduleRestart(unsigned long delayMs) {
  restartAtMs    = millis() + delayMs;
  restartPending = true;
  addLog("♻️ Restart scheduled - settings will apply cleanly...");
}

// ═════════════════════════════════════════════════════════════════════════
//  [FIX 8] CUSTOM ASYNC WIFI CONNECTION MANAGER
// ═════════════════════════════════════════════════════════════════════════

unsigned long wcmBackoffMs() {
  unsigned long d = WCM_RETRY_BASE << (gRetryCount > 4 ? 4 : gRetryCount);
  return (d > WCM_RETRY_MAX) ? WCM_RETRY_MAX : d;
}

void wcmEnter(WifiConnState s) { wcState = s; wcStateTime = millis(); }

void wcmOnOnline() {
  int tries = gRetryCount;
  wcmEnter(WCS_ONLINE);
  gReconnecting = false;
  gRetryCount   = 0;
  String msg = "✅ WiFi Connected! IP: " + WiFi.localIP().toString();
  if (tries > 0) msg += " (after " + String(tries) + " retries)";
  addLog(msg);
  // [FIX 12] Do NOT restart SoftAP on every STA connect - just keep AP enabled.
  // Restarting softAP here made the SSID vanish from phones.
  WiFi.enableAP(true);
  ensureHotspotAlive();
}

// Kick off one fresh non-blocking join attempt (the ESP-01 AP stays up)
void wcmStartAttempt() {
  if (cfgSSID == "") return;

  // [FIX 12] Wait for SoftAP beacons to settle before first STA radio use.
  // Immediate WiFi.begin()/disconnect right after softAP() kills visibility.
  if ((long)(millis() - apSettleUntil) < 0) {
    wcmEnter(WCS_RETRY_WAIT);
    return;
  }

  // Abort only the STA attempt - SoftAP must stay up (disconnect(false)).
  // Do NOT call setupHotspot() here - re-calling softAP() drops the beacon.
  WiFi.disconnect(false);
  WiFi.begin(cfgSSID.c_str(), cfgPass.c_str());
  gReconnecting = true;
  wcmEnter(WCS_CONNECTING);
}

void wifiManagerLoop() {
  unsigned long now = millis();
  if ((long)(now - wcNextPoll) < 0) return;
  wcNextPoll = now + WCM_POLL_MS;

  wifiIsUp = (WiFi.status() == WL_CONNECTED);

  // [FIX 12] SoftAP health + post-scan reassert (scan can mute SoftAP beacons)
  int scanState = WiFi.scanComplete();
  if (scanWasRunning && scanState != WIFI_SCAN_RUNNING) {
    scanWasRunning = false;
    ensureHotspotAlive();
  } else if (scanState == WIFI_SCAN_RUNNING) {
    scanWasRunning = true;
  }
  ensureHotspotAlive();

  switch (wcState) {

    case WCS_IDLE:
      if (wifiIsUp) wcmOnOnline();
      break;

    case WCS_CONNECTING:
      if (wifiIsUp) { wcmOnOnline(); break; }
      if (now - wcStateTime >= WCM_TIMEOUT_MS) {
        gRetryCount++;
        addLog("❌ WiFi join attempt #" + String(gRetryCount) +
               " timed out - next retry in " + String(wcmBackoffMs() / 1000) + "s");
        WiFi.disconnect(false);               // cleanly abort STA only, AP stays up
        wcmEnter(WCS_RETRY_WAIT);
      }
      break;

    case WCS_ONLINE:
      if (!wifiIsUp) {
        addLog("❌ ERROR: WiFi Disconnected from '" + cfgSSID + "'");
        if (cloudStarted) {
          RemoteXY.connect_flag = 0;
          rxyLastFlag = 255;
          rxySynced   = false;
        }
        if (cfgAutoConn) {
          addLog("🔄 Custom async reconnect loop engaged...");
          gReconnecting = true;
          gRetryCount   = 0;
          WiFi.disconnect(false);
          wcmEnter(WCS_RETRY_WAIT);
        } else {
          addLog("⏸ Auto-reconnect is OFF - use 🔄 Force Reconnect");
          wcmEnter(WCS_IDLE);
        }
      }
      break;

    case WCS_RETRY_WAIT:
      if (wifiIsUp) { wcmOnOnline(); break; }
      if (!cfgAutoConn) { wcmEnter(WCS_IDLE); break; }
      // Respect SoftAP settle window even during backoff
      if ((long)(now - apSettleUntil) < 0) break;
      if (now - wcStateTime >= wcmBackoffMs()) {
        addLog("🔄 Reconnect attempt #" + String(gRetryCount + 1) + " to " + cfgSSID);
        wcmStartAttempt();
      }
      break;
  }
}

// ── Hardware Control ──────────────────────────────────────────────────────────

void initRelayGPIO() {
  pinMode(PIN_SWITCH_01, INPUT_PULLUP);
  pinMode(PIN_SWITCH_02, INPUT_PULLUP);
  digitalWrite(PIN_SWITCH_01, HIGH);
  digitalWrite(PIN_SWITCH_02, HIGH);
  pinMode(PIN_SWITCH_01, OUTPUT);
  pinMode(PIN_SWITCH_02, OUTPUT);
  digitalWrite(PIN_SWITCH_01, HIGH);
  digitalWrite(PIN_SWITCH_02, HIGH);
}

void applyRelays() {
  digitalWrite(PIN_SWITCH_01, fanState   ? LOW : HIGH);
  digitalWrite(PIN_SWITCH_02, lightState ? LOW : HIGH);
}

void saveRelayStates() {
  relayStatesDirty = true;
  lastRelayChangeTime = millis();
}

void processEEPROMSave() {
  if (relayStatesDirty && millis() - lastRelayChangeTime > 5000) {
    EEPROM.write(ADDR_FAN_STATE,   fanState   ? 1 : 0);
    EEPROM.write(ADDR_LIGHT_STATE, lightState ? 1 : 0);
    EEPROM.write(ADDR_VALID, EEPROM_MAGIC);
    EEPROM.commit();
    relayStatesDirty = false;
    addLog("💾 Relays saved to flash");
  }
}

void loadRelayStates() {
  if (EEPROM.read(ADDR_VALID) == EEPROM_MAGIC) {
    fanState   = (EEPROM.read(ADDR_FAN_STATE)   == 1);
    lightState = (EEPROM.read(ADDR_LIGHT_STATE) == 1);
  }
}

// ── Debug Log ─────────────────────────────────────────────────────────────────
#define LOG_MAX 15
String debugLog[LOG_MAX];
int logHead = 0, logCount = 0;

void addLog(String line) {
  Serial.println(line);
  debugLog[logHead] = line;
  logHead = (logHead + 1) % LOG_MAX;
  if (logCount < LOG_MAX) logCount++;
}

String getLogHtml() {
  String s = "";
  s.reserve(1024);
  int count = logCount;
  int idx = (logHead - 1 + LOG_MAX) % LOG_MAX;
  while (count-- > 0) {
    String& l = debugLog[idx];
    String cls = "li";
    if (l.indexOf("✅") >= 0 || l.indexOf("🟢") >= 0 || l.indexOf("OK") >= 0) cls = "lok";
    else if (l.indexOf("❌") >= 0 || l.indexOf("🔴") >= 0 || l.indexOf("FAIL") >= 0) cls = "ler";
    else if (l.indexOf("⚠") >= 0 || l.indexOf("retry") >= 0 || l.indexOf("⏸") >= 0) cls = "lw";
    
    s += "<div class='" + cls + "'>" + l + "</div>\n";
    idx = (idx - 1 + LOG_MAX) % LOG_MAX;
  }
  return s == "" ? "<div class='lm'>No activity yet...</div>" : s;
}

// ── EEPROM Functions ──────────────────────────────────────────────────────────
void eepromSave(int addr, const String& s, int len) {
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + i, i < (int)s.length() ? s[i] : 0);
  }
  EEPROM.write(ADDR_VALID, EEPROM_MAGIC);
  EEPROM.commit();
}

String eepromLoad(int addr, int len) {
  String r = "";
  r.reserve(len);
  for (int i = 0; i < len; i++) {
    char c = EEPROM.read(addr + i);
    if (c == 0 || c < 32 || c > 126) break;
    r += c;
  }
  return r;
}

// ── [FIX 12] Strict Hotspot Setup (start once, keep visible) ──────────────────
void setupHotspot(bool forceRestart) {
  // Re-calling WiFi.softAP() on every reconnect makes the SSID disappear from
  // phone WiFi lists. Only (re)start when forced or when SoftAP is not up yet.
  if (apStarted && !forceRestart) {
    WiFi.enableAP(true);
    if (!(WiFi.getMode() & WIFI_AP)) {
      WiFi.mode(WIFI_AP_STA);
      WiFi.enableAP(true);
    }
    return;
  }

  WiFi.mode(WIFI_AP_STA);

  // Start SoftAP FIRST (channel 1 = best phone visibility), THEN apply IP config.
  // softAPConfig-before-softAP is flaky on some ESP8266 core builds.
  bool ok = WiFi.softAP(apSSID.c_str(), apPass.c_str(), AP_CHANNEL, false, 4);
  delay(50);  // brief settle only during start/credential change (not in loop)
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

  apStarted = ok && (WiFi.softAPIP()[0] != 0);
  apSettleUntil = millis() + AP_SETTLE_MS;

  if (apStarted) {
    addLog("📱 Hotspot ON: '" + apSSID + "'  pass: " + apPass +
           "  IP: " + WiFi.softAPIP().toString() + "  ch" + String(AP_CHANNEL));
  } else {
    addLog("❌ Hotspot FAILED to start - check power / antenna");
  }
}

void ensureHotspotAlive() {
  // softAP IP 0.0.0.0 => beacon died (common after scan / STA channel fights)
  bool dead = !apStarted || WiFi.softAPIP()[0] == 0 || !(WiFi.getMode() & WIFI_AP);
  if (!dead) {
    WiFi.enableAP(true);
    return;
  }
  // Throttle restarts so we never churn SoftAP the way the old reconnect loop did
  static unsigned long lastApRestart = 0;
  if (millis() - lastApRestart < 10000) return;
  lastApRestart = millis();
  addLog("⚠ Hotspot beacon lost - restarting SoftAP");
  setupHotspot(true);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SINRICPRO INITIALIZATION
// ═════════════════════════════════════════════════════════════════════════════
void startSinric() {
  if (sinricStarted) return; 
  if (spKey == "" || spSecret == "" || spFanId == "" || spLightId == "") {
    addLog("ℹ️  SinricPro: Credentials empty. Please setup.");
    return;
  }

  addLog("🔊 Starting SinricPro...");

  SinricPro.onConnected([]() {
    sinricOnline = true;
    addLog("🟢 SinricPro: ONLINE ✓");
  });
  
  SinricPro.onDisconnected([]() {
    sinricOnline = false;
    addLog("🔴 SinricPro: OFFLINE");
  });

  SinricProSwitch& fan = SinricPro[spFanId.c_str()];
  fan.onPowerState([](const String& id, bool& state) -> bool {
    fanState = state;
    if (cloudStarted) RemoteXY.switch_01 = fanState ? 1 : 0;
    applyRelays();
    saveRelayStates();
    addLog(fanState ? "🔊 Voice: Fan ON" : "🔇 Voice: Fan OFF");
    return true;
  });

  SinricProSwitch& light = SinricPro[spLightId.c_str()];
  light.onPowerState([](const String& id, bool& state) -> bool {
    lightState = state;
    if (cloudStarted) RemoteXY.switch_02 = lightState ? 1 : 0;
    applyRelays();
    saveRelayStates();
    addLog(lightState ? "💡 Voice: Light ON" : "🌑 Voice: Light OFF");
    return true;
  });

  SinricPro.restoreDeviceStates(false);
  SinricPro.begin(spKey.c_str(), spSecret.c_str());
  sinricStarted = true;
}

// ── RemoteXY Cloud ────────────────────────────────────────────────────────────
static CRemoteXYNet_WiFi* remoteXYNet = nullptr;
static RemoteXYGui*       remoteXYGui = nullptr;

void startCloud() {
  if (cloudStarted || cfgToken == "") return;
  addLog("☁️  Starting RemoteXY...");
  remoteXYNet = new CRemoteXYNet_WiFi(cfgSSID.c_str(), cfgPass.c_str());
  remoteXYGui = RemoteXYEngine.addGui(RemoteXY_CONF_PROGMEM, &RemoteXY);
  remoteXYGui->addConnectionCloud(remoteXYNet, REMOTEXY_CLOUD_SERVER, REMOTEXY_CLOUD_PORT, cfgToken.c_str());
  cloudStarted = true;
  
  rxyLastFlag    = 255;
  rxySynced      = false;
  rxyConnectTime = 0;
}

// ── Signal Bars ───────────────────────────────────────────────────────────────
int rssiToBars(int r){ 
  if(r >= -55) return 4;
  if(r >= -65) return 3;
  if(r >= -75) return 2;
  return 1; 
}

String barsHtml(int n){
  String s = "<span class='bars'>\n";
  int h[] = {5, 9, 13, 17};
  for(int i = 0; i < 4; i++){
    String c = (i < n) ? "#10b981" : "#2d3748";
    s += "  <span style='height:" + String(h[i]) + "px; background:" + c + "'></span>\n";
  }
  s += "</span>\n";
  return s;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PREMIUM UI CSS 
// ═════════════════════════════════════════════════════════════════════════════
const char CSS[] PROGMEM = R"CSS(
* { box-sizing: border-box; margin: 0; padding: 0; }
:root {
  --bg: #090d14; --sur: #111827; --card: #1a2235; --bdr: #2a3441;
  --grn: #10b981; --grn-glow: rgba(16,185,129,0.3);
  --blu: #3b82f6; --red: #ef4444; --ylw: #f59e0b;
  --txt: #f8fafc; --mut: #94a3b8;
  --fn: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  --mn: "SF Mono", Consolas, monospace;
}
body { font-family: var(--fn); background: var(--bg); color: var(--txt); font-size: 14px; line-height: 1.5; -webkit-font-smoothing: antialiased; }
a { text-decoration: none; }
nav { background: rgba(17, 24, 39, 0.85); backdrop-filter: blur(12px); border-bottom: 1px solid var(--bdr); position: sticky; top: 0; z-index: 50; }
.topbar { display: flex; justify-content: space-between; padding: 12px 16px; align-items: center; }
.badges { display: flex; gap: 8px; }
.badge { font-size: 11px; font-family: var(--mn); padding: 5px 10px; border-radius: 8px; font-weight: 700; display: flex; align-items: center; gap: 5px; border: 1px solid var(--bdr); color: var(--mut); background: var(--card); transition: all 0.3s ease; }
.badge.on { border-color: #10b981; color: #10b981; background: rgba(16, 185, 129, 0.15); box-shadow: 0 0 10px var(--grn-glow); }
.badge.err { border-color: #ef4444; color: #ef4444; background: rgba(239, 68, 68, 0.12); box-shadow: 0 0 10px rgba(239, 68, 68, 0.25); }
.tabs { display: flex; padding: 0 16px; gap: 8px; overflow-x: auto; }
.tabs::-webkit-scrollbar { display: none; }
.tab { padding: 10px 14px; color: var(--mut); font-size: 13px; font-weight: 600; white-space: nowrap; border-bottom: 2px solid transparent; transition: all 0.2s; }
.tab.active { color: var(--blu); border-bottom-color: var(--blu); }
.container { max-width: 500px; margin: 0 auto; padding: 20px 16px 80px; }
.card { background: var(--card); border-radius: 16px; border: 1px solid var(--bdr); margin-bottom: 20px; overflow: hidden; box-shadow: 0 4px 15px rgba(0, 0, 0, 0.2); }
.ch { padding: 16px; border-bottom: 1px solid var(--bdr); display: flex; align-items: center; gap: 12px; background: rgba(255, 255, 255, 0.02); }
.ci { width: 36px; height: 36px; border-radius: 10px; display: flex; align-items: center; justify-content: center; font-size: 18px; }
.cg { background: rgba(16, 185, 129, 0.15); color: var(--grn); }
.cb { background: rgba(59, 130, 246, 0.15); color: var(--blu); }
.cy { background: rgba(245, 158, 11, 0.15); color: var(--ylw); }
.cv { background: rgba(139, 92, 246, 0.15); color: #8b5cf6; }
.cl { font-weight: 700; font-size: 15px; }
.cd { font-size: 12px; color: var(--mut); margin-top: 2px; }
.cb-body { padding: 16px; }
.relay-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.relay-btn { background: rgba(255, 255, 255, 0.03); border: 1px solid var(--bdr); border-radius: 16px; padding: 20px 16px; display: flex; flex-direction: column; cursor: pointer; transition: all 0.2s; user-select: none; -webkit-user-select: none; }
.relay-btn:active { transform: scale(0.96); }
.relay-btn.on { background: rgba(16, 185, 129, 0.1); border-color: var(--grn); box-shadow: 0 4px 15px var(--grn-glow); }
.relay-icon { width: 42px; height: 42px; border-radius: 50%; display: flex; align-items: center; justify-content: center; font-size: 20px; margin-bottom: 12px; background: rgba(255, 255, 255, 0.05); transition: all 0.3s; }
.relay-btn.on .relay-icon { background: var(--grn); color: #fff; box-shadow: 0 0 15px var(--grn); }
.relay-name { font-weight: 600; font-size: 14px; margin-bottom: 4px; color: var(--txt); }
.relay-state { font-size: 12px; font-weight: 700; color: var(--mut); }
.relay-btn.on .relay-state { color: var(--grn); }
.fld { margin-bottom: 16px; }
.fld:last-child { margin-bottom: 0; }
.lbl { display: block; font-size: 11px; font-weight: 600; color: var(--mut); margin-bottom: 8px; text-transform: uppercase; letter-spacing: 0.5px; }
.iw { position: relative; display: flex; align-items: center; }
.pfx { position: absolute; left: 12px; font-size: 16px; color: var(--mut); }
.input { width: 100%; background: rgba(0, 0, 0, 0.2); border: 1px solid var(--bdr); border-radius: 10px; padding: 12px 12px 12px 38px; color: var(--txt); font-family: var(--fn); font-size: 14px; transition: border 0.2s; -webkit-appearance: none; }
.input:focus { border-color: var(--blu); box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.15); }
.eye { position: absolute; right: 8px; background: none; border: none; color: var(--mut); cursor: pointer; padding: 4px; font-size: 14px; }
.btn { width: 100%; padding: 14px; border-radius: 10px; font-size: 14px; font-weight: 700; border: none; cursor: pointer; display: flex; justify-content: center; align-items: center; gap: 8px; transition: all 0.2s; }
.btn:active { transform: scale(0.98); }
.bg { background: var(--blu); color: #fff; box-shadow: 0 4px 12px rgba(59, 130, 246, 0.3); }
.bg:hover { background: #2563eb; }
.bv { background: var(--grn); color: #fff; box-shadow: 0 4px 12px var(--grn-glow); }
.btn-out { background: transparent; border: 1px solid var(--bdr); color: var(--txt); }
.alert { padding: 12px 16px; border-radius: 10px; margin-bottom: 20px; font-size: 13px; font-weight: 500; display: flex; gap: 10px; align-items: center; }
.aok { background: rgba(16, 185, 129, 0.1); border: 1px solid rgba(16, 185, 129, 0.3); color: var(--grn); }
.aer { background: rgba(239, 68, 68, 0.1); border: 1px solid rgba(239, 68, 68, 0.3); color: var(--red); }
.av { background: rgba(139, 92, 246, 0.1); border: 1px solid rgba(139, 92, 246, 0.3); color: #8b5cf6; }
.wi { display: flex; align-items: center; justify-content: space-between; padding: 12px; border: 1px solid var(--bdr); border-radius: 10px; margin-bottom: 8px; cursor: pointer; background: rgba(255, 255, 255, 0.02); }
.wi:hover { border-color: var(--blu); background: rgba(59, 130, 246, 0.05); }
.wn { font-weight: 600; font-size: 14px; }
.wm { font-size: 11px; color: var(--mut); margin-top: 4px; }
.wr { display: flex; align-items: center; gap: 7px; }
.bdg { font-size: 10px; padding: 2px 6px; border-radius: 4px; font-weight: 600; }
.bs { background: rgba(16, 185, 129, 0.15); color: var(--grn); border: 1px solid rgba(16, 185, 129, 0.3); }
.bo { background: rgba(245, 158, 11, 0.15); color: var(--ylw); border: 1px solid rgba(245, 158, 11, 0.3); }
.bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 14px; vertical-align: middle; }
.bars span { width: 3px; border-radius: 2px; display: inline-block; }
.sg { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 20px; }
.sc { background: rgba(255, 255, 255, 0.03); padding: 14px; border-radius: 12px; border: 1px solid var(--bdr); }
.sl { font-size: 11px; color: var(--mut); text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px; }
.sv { font-weight: 700; font-size: 14px; font-family: var(--mn); }
.cg2 { color: var(--grn); } .cr { color: var(--red); } .cyw { color: var(--ylw); } .cv2 { color: #8b5cf6; } .cb2 { color: var(--blu); }
.lb { background: #05080c; border: 1px solid var(--bdr); border-radius: 12px; padding: 12px; font-family: var(--mn); font-size: 12px; line-height: 1.7; height: 250px; overflow-y: auto; }
.li { color: var(--mut); } .lok { color: var(--grn); } .ler { color: var(--red); } .lw { color: var(--ylw); }
.lb::-webkit-scrollbar { width: 4px; } .lb::-webkit-scrollbar-thumb { background: var(--bdr); border-radius: 2px; }

/* ── PREMIUM TOGGLE SWITCH CSS ── */
.switch-wrapper { display: flex; align-items: center; justify-content: space-between; padding: 14px; background: rgba(0,0,0,0.2); border-radius: 10px; border: 1px solid var(--bdr); margin-bottom: 16px; }
.switch-info { display: flex; flex-direction: column; }
.switch-title { font-size: 13px; font-weight: 600; color: var(--txt); }
.switch-desc { font-size: 11px; color: var(--mut); margin-top: 2px; }
.switch { position: relative; display: inline-block; width: 40px; height: 22px; }
.switch input { opacity: 0; width: 0; height: 0; }
.slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background-color: rgba(255,255,255,0.1); transition: .3s; border-radius: 34px; border: 1px solid var(--bdr); }
.slider:before { position: absolute; content: ""; height: 16px; width: 16px; left: 2px; bottom: 2px; background-color: var(--mut); transition: .3s; border-radius: 50%; }
input:checked + .slider { background-color: rgba(16,185,129,0.15); border-color: rgba(16,185,129,0.5); }
input:checked + .slider:before { transform: translateX(18px); background-color: var(--grn); box-shadow: 0 0 5px var(--grn); }
)CSS";

// ═════════════════════════════════════════════════════════════════════════════
//  JAVASCRIPT (LIVE ASYNC AJAX UI)
// ═════════════════════════════════════════════════════════════════════════════
const char JS[] PROGMEM = R"JS(
function tog(id, b) {
  var e = document.getElementById(id);
  e.type = (e.type === 'password') ? 'text' : 'password';
  b.textContent = (e.type === 'password') ? '👁' : '🙈';
}

function pick(ssid, locked) {
  document.getElementById('ssid').value = ssid;
  window.scrollTo({ top: document.getElementById('ssid').offsetTop - 100, behavior: 'smooth' });
  if(locked) document.getElementById('pass').focus();
}

function toggleRelay(device) {
  var btn = document.getElementById(device === 'fan' ? 'fanBtn' : 'lightBtn');
  if(btn) btn.style.opacity = '0.6';

  fetch('/relay?r=' + device)
    .then(r => r.json())
    .then(d => {
      var fanBtn = document.getElementById('fanBtn');
      if(fanBtn) {
        fanBtn.className = 'relay-btn' + (d.fan ? ' on' : '');
        fanBtn.querySelector('.relay-state').textContent = d.fan ? 'RUNNING' : 'OFF';
        fanBtn.style.opacity = '1';
      }
      var lightBtn = document.getElementById('lightBtn');
      if(lightBtn) {
        lightBtn.className = 'relay-btn' + (d.light ? ' on' : '');
        lightBtn.querySelector('.relay-state').textContent = d.light ? 'ON' : 'OFF';
        lightBtn.style.opacity = '1';
      }
    })
    .catch(function(e) {
      if(btn) btn.style.opacity = '1';
      console.log('Relay err:', e);
    });
}

function doConn(e) {
  e.preventDefault();
  var btn = document.getElementById('cbtn');
  btn.innerHTML = '⏳ Connecting in Background...';
  btn.style.opacity = '0.7';
  btn.style.pointerEvents = 'none';

  fetch('/connect', {
    method: 'POST',
    body: new URLSearchParams(new FormData(e.target))
  })
  .then(r => r.json())
  .then(d => {
    let checkAttempts = 0;
    var checkInt = setInterval(() => {
      fetch('/statdata').then(res=>res.json()).then(stat => {
        checkAttempts++;
        if(stat.wifi === 'Connected') {
          clearInterval(checkInt);
          btn.innerHTML = '✅ Connected Successfully!';
          btn.style.background = '#10b981';
        } else if (checkAttempts >= 8) {
          clearInterval(checkInt);
          btn.innerHTML = '⚡ Connect & Save';
          btn.style.opacity = '1';
          btn.style.pointerEvents = 'auto';
        }
      });
    }, 1500);
  }).catch(err => {
    btn.innerHTML = '⚡ Connect & Save';
    btn.style.opacity = '1';
    btn.style.pointerEvents = 'auto';
  });
}

var statFails = 0;

setInterval(() => {
  fetch('/statdata')
    .then(r => r.json())
    .then(d => {
      statFails = 0;
      var isConn = (d.wifi === 'Connected');
      var be = document.getElementById('bEsp');
      var bw = document.getElementById('bWifi');
      var bi = document.getElementById('bIp');
      
      if(be && bw && bi) {
        be.className = isConn ? 'badge on' : 'badge err';
        bw.className = isConn ? 'badge on' : 'badge err';
        bi.textContent = isConn ? d.ip : 'Disconnected';
      }

      var sb = document.getElementById('wifiStatusBar');
      if(sb) {
        if(isConn) {
          sb.className = 'alert aok';
          sb.innerHTML = '🟢 <b>WiFi Connected</b> — Local IP: <b>' + d.ip + '</b>';
        } else {
          var rtry = parseInt(d.retry, 10) || 0;
          var detail = (rtry > 0) ? ('auto-reconnect active (attempt #' + (rtry + 1) + ')')
                                  : 'waiting to connect — check router or use Force Reconnect';
          sb.className = 'alert aer';
          sb.innerHTML = '❌ <b>WiFi Disconnected</b> — ' + detail;
        }
      }

      if(document.getElementById('l1')) {
        document.getElementById('l1').setAttribute('stroke', d.lc1);
        if(d.wifi === 'Connected') {
           document.getElementById('l1').style.animation = 'none';
           document.getElementById('l1').removeAttribute('stroke-dasharray');
        }
        document.getElementById('c1').setAttribute('fill', d.lc1);
        
        document.getElementById('nEsp').setAttribute('stroke', d.espClr);
        document.getElementById('tEspClr').setAttribute('fill', d.espClr);
        document.getElementById('tEspLbl').setAttribute('fill', d.espClr);
        document.getElementById('tEspLbl').textContent = d.espLbl;

        document.getElementById('nRou').setAttribute('stroke', d.espClr);
        document.getElementById('tRouClr').setAttribute('fill', d.espClr);
        document.getElementById('tRouLbl').setAttribute('fill', d.espClr);
        document.getElementById('tRouLbl').textContent = d.wifiLbl;
        
        var showIp = d.ip === 'AP Mode' ? '' : d.ip;
        document.getElementById('tIp').textContent = showIp;

        document.getElementById('l2').setAttribute('stroke', d.lc2);
        if(d.cloud === 'Online') {
           document.getElementById('l2').style.animation = 'none';
           document.getElementById('l2').removeAttribute('stroke-dasharray');
        }
        document.getElementById('c2').setAttribute('fill', d.lc2);
        document.getElementById('nMob').setAttribute('stroke', d.lc2);
        document.getElementById('tMobClr').setAttribute('fill', d.mobClr);
        document.getElementById('tMobLbl').setAttribute('fill', d.mobClr);
        document.getElementById('tMobLbl').textContent = d.mobLbl;
        
        if(document.getElementById('pillWifi')) document.getElementById('pillWifi').textContent = isConn ? '✓ ON' : '✗ OFF';
        if(document.getElementById('pillRxy')) document.getElementById('pillRxy').textContent = d.cloud === 'Online' ? '✓ ON' : '✗ OFF';
        if(document.getElementById('pillSnrc')) document.getElementById('pillSnrc').textContent = d.sinric === 'Online' ? '✓ ON' : '✗ OFF';
      }

      if(window.location.pathname === '/status') {
        if(document.getElementById('swifi')) document.getElementById('swifi').textContent = d.wifi;
        if(document.getElementById('scld')) document.getElementById('scld').textContent = d.cloud;
        if(document.getElementById('ssnrc')) document.getElementById('ssnrc').textContent = d.sinric;
        if(document.getElementById('sip')) document.getElementById('sip').textContent = d.ip;
        if(document.getElementById('srssi')) document.getElementById('srssi').textContent = d.rssi ? d.rssi + " dBm" : "-";
        if(document.getElementById('sheap')) document.getElementById('sheap').textContent = d.heap ? (Math.round(parseInt(d.heap, 10) / 102.4) / 10) + ' KB' : '-';
        if(document.getElementById('sap')) document.getElementById('sap').textContent = d.ap || '-';
      }
    })
    .catch(() => {
      statFails++;
      if(statFails >= 3) {
        var sb = document.getElementById('wifiStatusBar');
        if(sb) {
          sb.className = 'alert aer';
          sb.innerHTML = '❌ <b>Cannot reach the ESP8266</b> — device offline or restarting. Try the hotspot at 192.168.4.1';
        }
      }
    });
}, 1000); 

if(window.location.pathname === '/status') {
  setInterval(() => {
    fetch('/logdata').then(r => r.text()).then(t => {
      var b = document.getElementById('lb'); if(b) b.innerHTML = t;
    });
  }, 2000);
}
)JS";

// ── Page Helpers ──────────────────────────────────────────────────────────────
void sendChunked(String& b) {
  int pos = 0, len = b.length();
  while (pos < len) { 
    server.sendContent(b.substring(pos, min(pos + 2000, len))); 
    pos += 2000; 
  }
}

void sendPageStart(const String& tab) {
  bool wc = (WiFi.status() == WL_CONNECTED);
  String bClass = wc ? "badge on" : "badge";
  
  String ipText = "AP Mode";
  if (wc) {
    IPAddress lip = WiFi.localIP();
    if (lip[0] != 0) ipText = lip.toString();
  }

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  
  String header = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'><title>Smart Home App</title><style>");
  server.sendContent(header);
  server.sendContent(FPSTR(CSS));
  
  String navOpen = F("</style></head><body><nav><div class='topbar'>");
  server.sendContent(navOpen);
  
  server.sendContent(F("<div style='font-weight:800;font-size:16px;color:#f8fafc'>SmartHome</div><div class='badges'>"));
  
  server.sendContent("<div id='bEsp' class='" + bClass + "'>⬛ ESP-01</div>\n");
  server.sendContent("<div id='bWifi' class='" + bClass + "'>📶 <span id='bIp'>" + ipText + "</span></div>\n");
  
  server.sendContent(F("</div></div><div class='tabs'>"));
  server.sendContent("<a href='/' class='tab " + String(tab == "wifi" ? "active" : "") + "'>🏠 Control & WiFi</a>\n");
  server.sendContent("<a href='/voice' class='tab " + String(tab == "voice" ? "active" : "") + "'>🎙️ Voice Setup</a>\n");
  server.sendContent("<a href='/status' class='tab " + String(tab == "status" ? "active" : "") + "'>📊 Diagnostics</a>\n");
  server.sendContent(F("</div></nav><div class='container'>\n"));
}

void sendPageEnd() {
  server.sendContent(F("</div>\n<script>\n")); 
  server.sendContent(FPSTR(JS));
  server.sendContent(F("\n</script>\n</body>\n</html>")); 
  server.sendContent("");
}

String banner(String cls, String msg) { 
  if (msg == "") return "";
  return "<div class='alert " + cls + "'>" + msg + "</div>\n"; 
}

// ── WiFi Scan List HTML Builder ───────────────────────────────────────────────
String wifiList() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return F("<div class='nn'>Scanning...</div>\n");
  if (n <= 0) return F("<div class='nn'><a href='/scan' style='color:var(--blu)'>Tap to scan</a></div>\n");
  
  String h = "";
  h.reserve(1024);
  for (int i = 0; i < n; i++) {
    String ss = WiFi.SSID(i); 
    int rs = WiFi.RSSI(i);
    bool lk = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    bool sv = (ss == cfgSSID);
    
    String sf = ss; 
    sf.replace("'", "\\'");
    
    h += "<div class='wi' onclick='pick(\"" + sf + "\"," + (lk ? "true" : "false") + ")'>\n";
    h += "  <div>\n";
    h += "    <div class='wn'>" + ss + "</div>\n";
    h += "    <div class='wm'>" + String(rs) + " dBm &nbsp;.&nbsp; ch " + String(WiFi.channel(i)) + "</div>\n";
    h += "  </div>\n";
    h += "  <div class='wr'>\n";
    
    if (sv) h += F("    <span class='bdg bs'>Saved</span>\n");
    if (!lk) h += F("    <span class='bdg bo'>Open</span>\n");
    else h += F("    <span style='font-size:12px'>🔒</span>\n");
    
    h += "    " + barsHtml(rssiToBars(rs));
    h += "  </div>\n";
    h += "</div>\n";
  }
  return h;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PREMIUM CONNECTION DIAGRAM (DYNAMIC SVG)
// ═════════════════════════════════════════════════════════════════════════════
String connDiagram() {
  bool wc  = (WiFi.status() == WL_CONNECTED);
  bool rc  = (cloudStarted && RemoteXY.connect_flag);
  bool snc = sinricOnline; 

  String lc1 = wc  ? "#10b981" : (gReconnecting ? "#f59e0b" : "#334155");
  String lc2 = rc  ? "#10b981" : "#334155";

  String espLbl = wc ? "Connected" : (gReconnecting ? "Retry #"+String(gRetryCount) : (cfgSSID!="" ? "Saved" : "Not set"));
  String espClr = wc ? "#10b981" : (gReconnecting ? "#f59e0b" : (cfgSSID!="" ? "#f59e0b" : "#ef4444"));

  String wifiLbl  = wc ? WiFi.SSID() : (cfgSSID!="" ? cfgSSID : "---");
  String mobLbl   = rc ? "App Online" : "App Offline";
  
  String ipTxt = "";
  if (wc) {
    IPAddress lip = WiFi.localIP();
    if (lip[0] != 0) ipTxt = lip.toString();
  }

  String d = "";
  d.reserve(2048);
  
  d += F("<div style='background:var(--card); border:1px solid var(--bdr); border-radius:16px; margin-bottom:20px; padding:20px 10px; box-shadow:0 4px 15px rgba(0,0,0,0.2)'>\n");
  d += F("  <svg viewBox='0 0 320 110' xmlns='http://www.w3.org/2000/svg' style='width:100%; max-width:360px; display:block; margin:0 auto'>\n");

  d += "    <line id='l1' x1='90' y1='50' x2='160' y2='50' stroke='" + lc1 + "' stroke-width='3' ";
  if (!wc && cfgSSID != "") d += "stroke-dasharray='6,4' style='animation:dash 1s linear infinite' ";
  d += "/>\n";
  d += "    <circle id='c1' cx='125' cy='50' r='5' fill='" + lc1 + "'/>\n";

  d += "    <line id='l2' x1='160' y1='50' x2='230' y2='50' stroke='" + lc2 + "' stroke-width='3' ";
  if (cloudStarted && !rc) d += "stroke-dasharray='6,4' style='animation:dash 1s linear infinite' ";
  d += "/>\n";
  d += "    <circle id='c2' cx='195' cy='50' r='5' fill='" + lc2 + "'/>\n";

  d += "    <circle id='nEsp' cx='55' cy='50' r='30' fill='#090d14' stroke='" + String(wc ? "#10b981" : "#334155") + "' stroke-width='2'/>\n";
  d += F("    <rect x='43' y='38' width='24' height='24' rx='4' fill='#1e293b' stroke='#475569' stroke-width='1'/>\n");
  d += "    <text x='55' y='53' text-anchor='middle' fill='" + String(wc ? "#10b981" : "#94a3b8") + "' font-size='8' font-family='monospace' font-weight='bold'>ESP</text>\n";

  d += "    <circle id='nRou' cx='160' cy='50' r='30' fill='#090d14' stroke='" + String(wc ? "#10b981" : "#ef4444") + "' stroke-width='2'/>\n";
  d += F("    <rect x='146' y='54' width='28' height='12' rx='3' fill='#1e293b' stroke='#475569' stroke-width='1'/>\n");
  d += F("    <line x1='152' y1='54' x2='148' y2='40' stroke='#64748b' stroke-width='2' stroke-linecap='round'/>\n");
  d += F("    <line x1='168' y1='54' x2='172' y2='40' stroke='#64748b' stroke-width='2' stroke-linecap='round'/>\n");
  d += "    <circle cx='153' cy='60' r='2' fill='" + String(wc ? "#10b981" : "#ef4444") + "'/>\n";

  d += "    <circle id='nMob' cx='265' cy='50' r='30' fill='#090d14' stroke='" + String(rc ? "#10b981" : "#334155") + "' stroke-width='2'/>\n";
  d += F("    <rect x='252' y='34' width='26' height='32' rx='4' fill='#1e293b' stroke='#475569' stroke-width='1'/>\n");
  d += F("    <rect x='255' y='37' width='20' height='22' rx='2' fill='#090d14'/>\n");
  d += "    <rect x='258' y='42' width='14' height='12' rx='2' fill='" + String(rc ? "#10b981" : "#334155") + "' opacity='0.8'/>\n";

  d += "    <text id='tEspClr' x='55' y='92' text-anchor='middle' fill='" + espClr + "' font-size='9' font-weight='bold'>ESP-01</text>\n";
  d += "    <text id='tEspLbl' x='55' y='104' text-anchor='middle' fill='" + espClr + "' font-size='8'>" + espLbl + "</text>\n";
  
  d += "    <text id='tRouClr' x='160' y='92' text-anchor='middle' fill='" + String(wc ? "#10b981" : "#ef4444") + "' font-size='9' font-weight='bold'>Router</text>\n";
  d += "    <text id='tRouLbl' x='160' y='104' text-anchor='middle' fill='" + String(wc ? "#10b981" : "#ef4444") + "' font-size='8'>" + wifiLbl + "</text>\n";
  
  d += "    <text id='tMobClr' x='265' y='92' text-anchor='middle' fill='" + String(rc ? "#10b981" : "#94a3b8") + "' font-size='9' font-weight='bold'>Mobile</text>\n";
  d += "    <text id='tMobLbl' x='265' y='104' text-anchor='middle' fill='" + String(rc ? "#10b981" : "#94a3b8") + "' font-size='8'>" + mobLbl + "</text>\n";

  d += "    <text id='tIp' x='125' y='38' text-anchor='middle' fill='#3b82f6' font-size='8' font-family='monospace'>" + ipTxt + "</text>\n";
  d += F("    <style>@keyframes dash{to{stroke-dashoffset:-20}}</style>\n");
  d += F("  </svg>\n");

  d += F("  <div style='display:flex; justify-content:space-around; margin-top:15px; gap:8px'>\n");
  d += "    <div style='flex:1; text-align:center; padding:8px 4px; border-radius:10px; border:1px solid " + String(wc ? "rgba(16,185,129,0.3)" : "rgba(245,158,11,0.2)") + "; background:" + String(wc ? "rgba(16,185,129,0.08)" : "rgba(245,158,11,0.05)") + ";'>\n";
  d += F("      <div style='font-size:10px; color:var(--mut); margin-bottom:2px'>WiFi</div>\n");
  d += "      <div id='pillWifi' style='font-size:12px; font-weight:700; color:" + lc1 + "'>" + String(wc ? "✓ ON" : (!wc && cfgSSID!="" ? "⏳ Retry" : "✗ OFF")) + "</div>\n";
  d += F("    </div>\n");
  
  d += "    <div style='flex:1; text-align:center; padding:8px 4px; border-radius:10px; border:1px solid " + String(rc ? "rgba(16,185,129,0.3)" : "rgba(100,116,139,0.2)") + "; background:" + String(rc ? "rgba(16,185,129,0.08)" : "rgba(100,116,139,0.05)") + ";'>\n";
  d += F("      <div style='font-size:10px; color:var(--mut); margin-bottom:2px'>RemoteXY</div>\n");
  d += "      <div id='pillRxy' style='font-size:12px; font-weight:700; color:" + lc2 + "'>" + String(rc ? "✓ ON" : (cloudStarted ? "⏳ Wait" : "✗ OFF")) + "</div>\n";
  d += F("    </div>\n");
  
  d += "    <div style='flex:1; text-align:center; padding:8px 4px; border-radius:10px; border:1px solid " + String(snc ? "rgba(139,92,246,0.3)" : "rgba(100,116,139,0.2)") + "; background:" + String(snc ? "rgba(139,92,246,0.08)" : "rgba(100,116,139,0.05)") + ";'>\n";
  d += F("      <div style='font-size:10px; color:var(--mut); margin-bottom:2px'>Voice</div>\n");
  d += "      <div id='pillSnrc' style='font-size:12px; font-weight:700; color:" + String(snc ? "#8b5cf6" : "#64748b") + "'>" + String(snc ? "✓ ON" : (sinricStarted ? "⏳ Wait" : "✗ OFF")) + "</div>\n";
  d += F("    </div>\n");
  d += F("  </div>\n");
  d += F("</div>\n");
  
  return d;
}

// ═════════════════════════════════════════════════════════════════════════════
//  PAGE: WIFI & DASHBOARD
// ═════════════════════════════════════════════════════════════════════════════
void sendWifiPage(String acls="", String amsg="") {
  bool wc = (WiFi.status() == WL_CONNECTED);
  String b = F("<div class='pg'>\n");
  b.reserve(4096);
  
  b += banner(acls, amsg);
  b += connDiagram();

  if (wc) {
    IPAddress lip = WiFi.localIP();
    String ipT = (lip[0] != 0) ? lip.toString() : String("...");
    b += "<div id='wifiStatusBar' class='alert aok'>🟢 <b>WiFi Connected</b> — Local IP: <b>" + ipT + "</b></div>\n";
  } else {
    b += F("<div id='wifiStatusBar' class='alert aer'>❌ <b>WiFi Disconnected</b> — waiting for connection...</div>\n");
  }

  // [FIX 12] Always show SoftAP join hint so phones know what to look for
  b += "<div class='alert av'>📱 <b>ESP Hotspot:</b> connect phone WiFi to <b>" + apSSID +
       "</b> (pass: <b>" + apPass + "</b>) then open <b>192.168.4.1</b></div>\n";

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci cb'>⚡</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>Smart Controls</div>\n");
  b += F("      <div class='cd'>Tap to toggle devices instantly</div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("  <div class='cb-body'>\n");
  b += F("    <div class='relay-grid'>\n");
  
  b += "      <div id='fanBtn' class='relay-btn " + String(fanState ? "on" : "") + "' onclick='toggleRelay(\"fan\")'>\n";
  b += F("        <div class='relay-icon'>🌀</div>\n");
  b += F("        <div class='relay-name'>Ceiling Fan</div>\n");
  b += "        <div class='relay-state'>" + String(fanState ? "RUNNING" : "OFF") + "</div>\n";
  b += F("      </div>\n");
  
  b += "      <div id='lightBtn' class='relay-btn " + String(lightState ? "on" : "") + "' onclick='toggleRelay(\"light\")'>\n";
  b += F("        <div class='relay-icon'>💡</div>\n");
  b += F("        <div class='relay-name'>Room Light</div>\n");
  b += "        <div class='relay-state'>" + String(lightState ? "ON" : "OFF") + "</div>\n";
  b += F("      </div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  if (!wc && cfgSSID != "") {
    b += F("<div style='display:flex; gap:10px; margin-bottom:20px'>\n");
    b += F("  <a href='/reconnect' class='btn btn-out' style='color:var(--ylw); border-color:rgba(245,158,11,0.3); background:rgba(245,158,11,0.05)'>🔄 Force Reconnect</a>\n");
    b += F("</div>\n");
  }

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci cy'>📶</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>WiFi Network</div>\n");
  b += F("      <div class='cd'>Connect your ESP to router</div>\n");
  b += F("    </div>\n");
  b += F("    <a href='/scan' class='btn btn-out' style='width:auto; padding:6px 12px; font-size:12px'>🔄 Scan</a>\n");
  b += F("  </div>\n");
  b += F("  <div class='cb-body'>\n");
  b += wifiList();
  
  b += F("    <form id='wform' onsubmit='doConn(event)'>\n");
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Network SSID</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>📡</span>\n");
  b += "          <input type='text' name='ssid' id='ssid' class='input' placeholder='Tap above or type' value='" + cfgSSID + "'>\n";
  b += F("        </div>\n");
  b += F("      </div>\n");
  
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Password</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("        <span class='pfx'>🔑</span>\n");
  b += F("        <input type='password' name='pass' id='pass' class='input' placeholder='WiFi password'>\n");
  b += F("        <button type='button' class='eye' onclick='tog(\"pass\",this)'>👁</button>\n");
  b += F("        </div>\n");
  b += F("      </div>\n");
  
  b += F("      <div class='switch-wrapper'>\n");
  b += F("        <div class='switch-info'>\n");
  b += F("          <div class='switch-title'>Auto Reconnect</div>\n");
  b += F("          <div class='switch-desc'>Connect automatically on restart</div>\n");
  b += F("        </div>\n");
  b += F("        <label class='switch'>\n");
  b += "          <input type='checkbox' name='autoconn' value='1' " + String(cfgAutoConn ? "checked" : "") + ">\n";
  b += F("          <span class='slider'></span>\n");
  b += F("        </label>\n");
  b += F("      </div>\n");

  b += F("      <button type='submit' id='cbtn' class='btn bg'>⚡ Connect & Save</button>\n");
  b += F("    </form>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci' style='background:rgba(139,92,246,0.15);color:#8b5cf6'>📱</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>Hotspot Settings</div>\n");
  b += F("      <div class='cd'>Change ESP-01 Name & Password</div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("  <div class='cb-body'>\n");
  b += F("    <form method='POST' action='/saveap'>\n");
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Hotspot Name (SSID)</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>📱</span>\n");
  b += "          <input type='text' name='ap_ssid' class='input' value='" + apSSID + "'>\n";
  b += F("        </div>\n");
  b += F("      </div>\n");
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Hotspot Password (Min 8 chars)</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("        <span class='pfx'>🔑</span>\n");
  b += "        <input type='password' name='ap_pass' id='ap_pass' class='input' value='" + apPass + "'>\n";
  b += F("        <button type='button' class='eye' onclick='tog(\"ap_pass\",this)'>👁</button>\n");
  b += F("        </div>\n");
  b += F("      </div>\n");
  b += F("      <button type='submit' class='btn btn-out' style='border-color:rgba(139,92,246,0.5);color:#8b5cf6'>💾 Save Hotspot</button>\n");
  b += F("    </form>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci cg'>☁️</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>RemoteXY App</div>\n");
  b += F("      <div class='cd'>Mobile app integration</div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("  <div class='cb-body'>\n");
  b += F("    <form method='POST' action='/savetoken'>\n");
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Cloud Token</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>☁️</span>\n");
  b += "          <input type='text' name='token' class='input' value='" + cfgToken + "'>\n");
  b += F("        </div>\n");
  b += F("      </div>\n");
  b += F("      <button type='submit' class='btn btn-out'>💾 Save Token</button>\n");
  b += F("    </form>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  b += F("</div>\n"); 

  sendPageStart("wifi");
  sendChunked(b);
  sendPageEnd();
}

void sendVoicePage(String acls="", String amsg="") {
  String b = F("<div class='pg'>\n");
  b.reserve(2048);
  
  b += banner(acls, amsg);

  if (sinricOnline) {
    b += F("<div class='alert aok' style='justify-content:center; font-size:14px'>\n");
    b += F("  🟢 <b>SinricPro is Online & Active!</b>\n");
    b += F("</div>\n");
  } else {
    b += F("<div class='alert av'>\n");
    b += F("  ℹ️ Fill credentials below. It applies instantly without resetting!\n");
    b += F("</div>\n");
  }

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci cv'>🎙️</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>Google Assistant / Alexa</div>\n");
  b += F("      <div class='cd'>Powered by SinricPro</div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  
  b += F("  <div class='cb-body'>\n");
  b += F("    <form method='POST' action='/savesinric'>\n");
  
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>App Key</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>🗝️</span>\n");
  b += "          <input type='text' name='spkey' class='input' value='" + spKey + "' placeholder='xxxxxxxx-xxxx...'>\n";
  b += F("        </div>\n");
  b += F("      </div>\n");

  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>App Secret</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>🔐</span>\n");
  b += "          <input type='password' name='spsec' id='spsec' class='input' value='" + spSecret + "'>\n";
  b += F("          <button type='button' class='eye' onclick='tog(\"spsec\",this)'>👁</button>\n");
  b += F("        </div>\n");
  b += F("      </div>\n");
  
  b += F("      <div class='hr'></div>\n");
  
  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Fan Device ID</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>🌀</span>\n");
  b += "          <input type='text' name='spfan' class='input' value='" + spFanId + "'>\n";
  b += F("        </div>\n");
  b += F("      </div>\n");

  b += F("      <div class='fld'>\n");
  b += F("        <label class='lbl'>Light Device ID</label>\n");
  b += F("        <div class='iw'>\n");
  b += F("          <span class='pfx'>💡</span>\n");
  b += "          <input type='text' name='splight' class='input' value='" + spLightId + "'>\n";
  b += F("        </div>\n");
  b += F("      </div>\n");

  b += F("      <button type='submit' class='btn bv' style='margin-top:10px'>💾 Save & Apply</button>\n");
  b += F("    </form>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  b += F("</div>\n"); 

  sendPageStart("voice");
  sendChunked(b);
  sendPageEnd();
}

void sendStatusPage() {
  bool wc = (WiFi.status() == WL_CONNECTED);
  
  String ipStr = "-";
  if (wc) {
    IPAddress lip = WiFi.localIP();
    if (lip[0] != 0) ipStr = lip.toString();
  }

  String b = F("<div class='pg'>\n");
  b.reserve(2048);

  if (wc) {
    b += "<div id='wifiStatusBar' class='alert aok'>🟢 <b>WiFi Connected</b> — Local IP: <b>" + ipStr + "</b></div>\n";
  } else {
    b += F("<div id='wifiStatusBar' class='alert aer'>❌ <b>WiFi Disconnected</b> — see the terminal log below</div>\n");
  }

  b += F("<div class='sg'>\n");
  b += "  <div class='sc'><div class='sl'>WiFi</div><div class='sv " + String(wc ? "cg2" : "cr") + "' id='swifi'>" + String(wc ? "Connected" : "Offline") + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>RemoteXY</div><div class='sv " + String((cloudStarted && RemoteXY.connect_flag) ? "cg2" : "cyw") + "' id='scld'>" + String((cloudStarted && RemoteXY.connect_flag) ? "Online" : (cloudStarted ? "Wait" : "Offline")) + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>Voice</div><div class='sv " + String(sinricOnline ? "cv2" : "cyw") + "' id='ssnrc'>" + String(sinricOnline ? "Online" : (sinricStarted ? "Wait" : "Offline")) + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>IP Address</div><div class='sv cb2' id='sip'>" + ipStr + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>Signal</div><div class='sv cb2' id='srssi'>" + String(wc ? String(WiFi.RSSI()) + " dBm" : "-") + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>Auto Reconnect</div><div class='sv cyw'>" + String(cfgAutoConn ? "ON (custom)" : "OFF") + "</div></div>\n";
  b += "  <div class='sc'><div class='sl'>Free Heap</div><div class='sv cb2' id='sheap'>" + String((ESP.getFreeHeap() + 512) / 1024) + " KB</div></div>\n";
  b += "  <div class='sc'><div class='sl'>ESP Hotspot</div><div class='sv " + String(apStarted && WiFi.softAPIP()[0] != 0 ? "cg2" : "cr") + "' id='sap'>" + apSSID + " @ " + WiFi.softAPIP().toString() + "</div></div>\n";
  b += F("</div>\n");

  b += F("<div class='card'>\n");
  b += F("  <div class='ch'>\n");
  b += F("    <div class='ci' style='background:rgba(255,255,255,0.05)'>🖥️</div>\n");
  b += F("    <div>\n");
  b += F("      <div class='cl'>System Terminal</div>\n");
  b += F("      <div class='cd'>Live updating logs</div>\n");
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("  <div class='cb-body'>\n");
  b += F("    <div class='lb' id='lb'>\n");
  b += getLogHtml();
  b += F("    </div>\n");
  b += F("  </div>\n");
  b += F("</div>\n");

  b += F("<a href='/reset' onclick='return confirm(\"Wipe completely?\")' class='btn btn-out' style='border-color:rgba(239,68,68,0.3); color:var(--red)'>🗑️ Factory Reset EEPROM</a>\n");
  b += F("</div>\n");

  sendPageStart("status");
  sendChunked(b);
  sendPageEnd();
}

// ═════════════════════════════════════════════════════════════════════════════
//  API & POST HANDLERS
// ═════════════════════════════════════════════════════════════════════════════
void handleRoot()   { sendWifiPage(); }
void handleVoice()  { sendVoicePage(); }
void handleStatus() { sendStatusPage(); }
void handleLogData(){ server.send(200, "text/html", getLogHtml()); }

void handleScan() {
  // [FIX 12] WiFi scan can mute SoftAP beacons on ESP8266 — mark running so
  // wifiManagerLoop re-asserts SoftAP when the scan finishes.
  scanWasRunning = true;
  WiFi.scanDelete(); 
  WiFi.scanNetworks(true);
  server.sendHeader("Location", "/"); 
  server.send(302);
}

void handleConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  
  cfgAutoConn = server.hasArg("autoconn");
  
  if (ssid != "") {
    bool credsChanged = (ssid != cfgSSID) || (pass != cfgPass);
    cfgSSID = ssid; 
    cfgPass = pass;
    eepromSave(ADDR_SSID, cfgSSID, 32);
    eepromSave(ADDR_PASS, cfgPass, 32);
    
    EEPROM.write(ADDR_AUTO_CONN, cfgAutoConn ? 1 : 0);
    EEPROM.commit();

    addLog(cfgAutoConn ? "ℹ️ Custom async reconnect enabled" : "ℹ️ Custom async reconnect disabled");

    gRetryCount = 0;
    // [FIX 12] Keep SoftAP up — do not restart it around STA join
    ensureHotspotAlive();
    wcmStartAttempt();
    addLog("📶 Connecting to: " + cfgSSID + " (async)");

    server.send(200, "application/json", "{\"ok\":true, \"msg\":\"Connecting...\"}");

    if (credsChanged && cloudStarted) scheduleRestart(1500);
  } else { 
    server.send(400, "application/json", "{\"ok\":false}"); 
  }
}

void handleSaveAP() {
  String s = server.arg("ap_ssid");
  String p = server.arg("ap_pass");
  if(s.length() >= 1 && p.length() >= 8) {
    apSSID = s; 
    apPass = p;
    eepromSave(ADDR_AP_SSID, apSSID, 32);
    eepromSave(ADDR_AP_PASS, apPass, 32);
    setupHotspot(true);   // [FIX 12] credential change is the only forced SoftAP restart
    sendWifiPage("aok", "✅ Hotspot Settings Saved! Look for '" + apSSID + "' on your phone");
  } else {
    sendWifiPage("aer", "❌ Name too short or Password < 8 chars!");
  }
}

void handleSaveToken() {
  cfgToken = server.arg("token");
  eepromSave(ADDR_TOKEN, cfgToken, 48);
  sendWifiPage("aok", "✅ Token Saved! Rebooting to apply...");
  scheduleRestart(1500);
}

void handleSaveSinric() {
  spKey = server.arg("spkey"); 
  spSecret = server.arg("spsec");
  spFanId = server.arg("spfan"); 
  spLightId = server.arg("splight");

  if(spSecret.length() < 30) {
    sendVoicePage("aer", "❌ App Secret looks too short! Paste the full secret.");
    return;
  }

  eepromSave(ADDR_SP_KEY, spKey, 36);
  eepromSave(ADDR_SP_SEC, spSecret, 128); 
  eepromSave(ADDR_SP_FAN, spFanId, 26);
  eepromSave(ADDR_SP_LIGHT, spLightId, 26);

  addLog("💾 Voice IDs updated!");

  String html = F("<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<style>body{background:#090d14;color:#10b981;font-family:-apple-system,sans-serif;display:flex;flex-direction:column;align-items:center;justify-content:center;height:100vh;margin:0;}");
  html += F(".loader{border:4px solid #1e293b;border-top:4px solid #10b981;border-radius:50%;width:50px;height:50px;animation:spin 1s linear infinite;margin-bottom:20px;}");
  html += F("@keyframes spin{0%{transform:rotate(0deg);} 100%{transform:rotate(360deg);}}</style></head>");
  html += F("<body><div class='loader'></div><h2 style='margin:0 0 10px 0'>Applying Settings...</h2>");
  html += F("<p style='color:#94a3b8;font-size:14px'>Re-connecting Voice Engine</p>");
  html += F("<script>setTimeout(()=>{window.location.href='/voice';}, 6000);</script></body></html>");
  
  server.send(200, "text/html", html);
  scheduleRestart(1500);
}

void handleStatData() {
  bool wc  = (WiFi.status() == WL_CONNECTED);
  bool rc  = (cloudStarted && RemoteXY.connect_flag);
  bool snc = sinricOnline;

  String ipStr = "AP Mode";
  if (wc) {
    IPAddress lip = WiFi.localIP();
    if (lip[0] != 0) ipStr = lip.toString();
  }

  String lc1 = wc ? "#10b981" : (cfgSSID!="" ? "#f59e0b" : "#334155");
  String lc2 = rc ? "#10b981" : "#334155";
  String espLbl = wc ? "Connected" : (cfgSSID!="" ? "Connecting" : "Not set");
  String espClr = wc ? "#10b981" : (cfgSSID!="" ? "#f59e0b" : "#ef4444");
  String wifiLbl = wc ? WiFi.SSID() : (cfgSSID!="" ? cfgSSID : "---");
  String mobLbl = rc ? "App Online" : "App Offline";
  
  char rssiBuf[8];
  if (wc) snprintf(rssiBuf, sizeof(rssiBuf), "%d", (int)WiFi.RSSI());
  else    rssiBuf[0] = '\0';

  String apInfo = apSSID + " / " + WiFi.softAPIP().toString();

  char js[520];
  snprintf(js, sizeof(js), "{\"wifi\":\"%s\",\"cloud\":\"%s\",\"sinric\":\"%s\",\"ip\":\"%s\",\"rssi\":\"%s\",\"retry\":\"%d\",\"lc1\":\"%s\",\"lc2\":\"%s\",\"espLbl\":\"%s\",\"espClr\":\"%s\",\"wifiLbl\":\"%s\",\"mobLbl\":\"%s\",\"mobClr\":\"%s\",\"heap\":\"%u\",\"ap\":\"%s\"}",
    (wc ? "Connected" : "Disconnected"),
    (cloudStarted ? (RemoteXY.connect_flag ? "Online" : "Connecting...") : "Offline"),
    (sinricOnline ? "Online" : (sinricStarted ? "Connecting..." : "No Creds")),
    ipStr.c_str(),
    rssiBuf,
    gRetryCount,
    lc1.c_str(),
    lc2.c_str(),
    espLbl.c_str(),
    espClr.c_str(),
    wifiLbl.c_str(),
    mobLbl.c_str(),
    (rc ? "#10b981" : "#94a3b8"),
    (unsigned)ESP.getFreeHeap(),
    apInfo.c_str()
  );
  
  server.send(200, "application/json", js);
}

void handleRelayToggle() {
  String w = server.arg("r");
  if (w == "fan") {
    fanState = !fanState;
    if (cloudStarted) RemoteXY.switch_01 = fanState ? 1 : 0;
    if (sinricStarted && spFanId != "") { 
      SinricProSwitch& f = SinricPro[spFanId.c_str()]; 
      f.sendPowerStateEvent(fanState); 
    }
    addLog(fanState ? "👆 Fan ON" : "👆 Fan OFF");
  } else if (w == "light") {
    lightState = !lightState;
    if (cloudStarted) RemoteXY.switch_02 = lightState ? 1 : 0;
    if (sinricStarted && spLightId != "") { 
      SinricProSwitch& l = SinricPro[spLightId.c_str()]; 
      l.sendPowerStateEvent(lightState); 
    }
    addLog(lightState ? "👆 Light ON" : "👆 Light OFF");
  }
  
  applyRelays();
  saveRelayStates();
  
  char js2[64];
  snprintf(js2, sizeof(js2), "{\"fan\":%s,\"light\":%s}", 
           fanState ? "true" : "false", 
           lightState ? "true" : "false");
  server.send(200, "application/json", js2);
}

void handleReconnect() {
  if (cfgSSID != "") {
    addLog("🔄 Manual Reconnect initiated");
    gRetryCount = 0;
    ensureHotspotAlive();
    wcmStartAttempt();
  }
  server.sendHeader("Location", "/"); 
  server.send(302);
}

void handleReset() {
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  
  String html = "<h2 style='color:#10b981;text-align:center;margin-top:20%'>✅ Factory Reset Complete! Restarting...</h2>";
  html += "<script>setTimeout(()=>{window.location.href='/';},3000);</script>";
  
  server.send(200, "text/html", html);
  scheduleRestart(800);
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  initRelayGPIO();

  Serial.begin(115200);
  delay(200);

  lastResetReason = ESP.getResetReason();

  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(ADDR_VALID) == EEPROM_MAGIC) {
    cfgSSID   = eepromLoad(ADDR_SSID, 32);
    cfgPass   = eepromLoad(ADDR_PASS, 32);
    String t  = eepromLoad(ADDR_TOKEN, 48); 
    if (t != "") cfgToken = t;
    
    spKey     = eepromLoad(ADDR_SP_KEY, 36);
    spSecret  = eepromLoad(ADDR_SP_SEC, 128); 
    spFanId   = eepromLoad(ADDR_SP_FAN, 26);
    spLightId = eepromLoad(ADDR_SP_LIGHT, 26);
    
    uint8_t ac = EEPROM.read(ADDR_AUTO_CONN);
    cfgAutoConn = (ac != 0); 

    String tAP = eepromLoad(ADDR_AP_SSID, 32);
    if(tAP.length() >= 1) apSSID = tAP;
    String tPass = eepromLoad(ADDR_AP_PASS, 32);
    if(tPass.length() >= 8) apPass = tPass;

    loadRelayStates();

    addLog("💾 Settings loaded. Reset: " + lastResetReason);
  } else {
    addLog("⚠️ First Boot - Please Configure at 192.168.4.1");
  }

  applyRelays();
  addLog("🔁 Relays initialised: Fan " + String(fanState ? "ON" : "OFF") +
         ", Light " + String(lightState ? "ON" : "OFF"));

  WiFi.persistent(false); 
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);
  
  // [FIX 12] Start SoftAP FIRST and let beacons settle before any STA join/scan.
  // Previously WiFi.disconnect()+WiFi.begin() ran immediately and the phone
  // never saw "ESP_SmartHome" in the WiFi list.
  setupHotspot(true);

  if (cfgSSID != "") {
    if (cfgAutoConn) {
      addLog("📶 Auto-connect scheduled after SoftAP settle (" +
             String(AP_SETTLE_MS) + "ms) -> " + cfgSSID);
      // Enter retry-wait so first join happens after SoftAP settle + short backoff
      gRetryCount = 0;
      gReconnecting = true;
      wcmEnter(WCS_RETRY_WAIT);
      // Make backoff effectively ~ SoftAP settle (not a full 5s) on first boot join
      wcStateTime = millis() - wcmBackoffMs() + AP_SETTLE_MS + 200;
      wcNextPoll = millis() + 500;
    } else {
      addLog("⏸ Auto-connect disabled by user.");
    }
  }

  if (cfgSSID != "" && spKey != "") {
    startSinric();
  }

  // [FIX 12] Do NOT scan at boot — scan mutes SoftAP and hides it from phones.
  // User can tap Scan later from the WiFi page.
  // WiFi.scanNetworks(true);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/voice", HTTP_GET, handleVoice);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/scan", HTTP_GET, handleScan);
  server.on("/connect", HTTP_POST, handleConnect);
  server.on("/saveap", HTTP_POST, handleSaveAP);
  server.on("/savetoken", HTTP_POST, handleSaveToken);
  server.on("/savesinric", HTTP_POST, handleSaveSinric);
  server.on("/relay", HTTP_GET, handleRelayToggle);
  server.on("/reconnect", HTTP_GET, handleReconnect);
  server.on("/statdata", HTTP_GET, handleStatData);
  server.on("/logdata", HTTP_GET, handleLogData);
  server.on("/reset", HTTP_GET, handleReset);
  server.begin();

  addLog("🚀 Hotspot '" + apSSID + "' ready — open http://192.168.4.1");
}

// ═════════════════════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();
  yield();

  wifiManagerLoop();

  bool wifiNow = (WiFi.status() == WL_CONNECTED);

  if (wifiNow && !cloudStarted && cfgToken != "") {
    startCloud();
  }

  if (wifiNow && !sinricStarted && spKey != "") {
    startSinric();
  }

  if (wifiNow) {
    if (sinricStarted) {
      SinricPro.handle();
    }

    if (cloudStarted) {
      RemoteXYEngine.handler();

      if (RemoteXY.connect_flag != rxyLastFlag) {
        rxyLastFlag = RemoteXY.connect_flag;
        if (RemoteXY.connect_flag) {
          addLog("✅ RemoteXY: ONLINE - Syncing states to app");
          rxyConnectTime = millis();
          rxySynced = false;
          RemoteXY.switch_01 = fanState ? 1 : 0;
          RemoteXY.switch_02 = lightState ? 1 : 0;
        } else {
          addLog("⚠️ RemoteXY: Offline");
          rxySynced = false;
        }
      }

      if (RemoteXY.connect_flag && !rxySynced) {
        if (millis() - rxyConnectTime < RXY_SYNC_DELAY) {
          RemoteXY.switch_01 = fanState ? 1 : 0;
          RemoteXY.switch_02 = lightState ? 1 : 0;
        } else {
          rxySynced = true;
          addLog("✅ RemoteXY: State sync complete, ready for commands");
        }
      }

      if (RemoteXY.connect_flag && rxySynced) {
        bool nf = (RemoteXY.switch_01 == 1);
        bool nl = (RemoteXY.switch_02 == 1);

        if (nf != fanState) {
          fanState = nf;
          addLog(nf ? "🔊 App: Fan ON" : "🔇 App: Fan OFF");
          if (sinricStarted && spFanId != "") {
            SinricProSwitch& f = SinricPro[spFanId.c_str()];
            f.sendPowerStateEvent(fanState);
          }
          saveRelayStates();
        }
        if (nl != lightState) {
          lightState = nl;
          addLog(nl ? "💡 App: Light ON" : "🌑 App: Light OFF");
          if (sinricStarted && spLightId != "") {
            SinricProSwitch& l = SinricPro[spLightId.c_str()];
            l.sendPowerStateEvent(lightState);
          }
          saveRelayStates();
        }
      }
    }
  }

  processEEPROMSave();

  static unsigned long lastGpioAssert = 0;
  if (millis() - lastGpioAssert >= 1000) {
    lastGpioAssert = millis();
    applyRelays();
  }

  if (restartPending && (long)(millis() - restartAtMs) >= 0) {
    if (relayStatesDirty) {
      lastRelayChangeTime = 0;
      processEEPROMSave();
    }
    delay(100);
    ESP.restart();
  }
}
