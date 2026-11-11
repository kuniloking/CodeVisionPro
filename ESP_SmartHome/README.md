# ESP8266 Smart Home (RemoteXY + SinricPro)

Flash `ESP_SmartHome.ino` to an **ESP-01 / ESP8266** with the RemoteXY and SinricPro libraries installed.

## FIX 12 — Hotspot not showing on phone

**Problem:** `ESP_SmartHome` SoftAP often never appeared in the phone WiFi list.

**Cause:** Every WiFi reconnect called `WiFi.disconnect()` + `WiFi.softAP()` again, and boot immediately started a STA join/scan. That stops SoftAP beacons, so phones cannot see the hotspot.

**Fix:**
- SoftAP starts **once** at boot (channel 1, max 4 clients)
- Reconnect loop no longer restarts SoftAP
- ~1s SoftAP settle delay before first router join
- No WiFi scan at boot (Scan button still works; SoftAP is re-asserted after scan)
- Beacon health-check only restarts SoftAP if it actually died

## Connect from phone

1. Power the ESP-01
2. On phone WiFi, join **`ESP_SmartHome`** / password **`12345678`**
3. Open **http://192.168.4.1**
