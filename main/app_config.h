#pragma once

// ============================================================================
// CONFIGURARE ESP32-S3 DevKitC-1 N16R8 (Bridge Deye -> Server Web)
// Aceste valori sunt folosite doar ca valori implicite la prima pornire.
// Dupa scriere, toate setarile pot fi schimbate din Portalul Wi-Fi
// ("Deye-ESP32-Setup" -> http://192.168.4.1) sau din butonul Setari din web.
// ============================================================================

// 1. Retea Wi-Fi de la locatia invertorului (implicit gol -> porneste Portalul
// Wi-Fi)
#define BRIDGE_WIFI_SSID ""
#define BRIDGE_WIFI_PASSWORD ""

// 2. Datele Loggerului Deye (stick-ul Wi-Fi Solarman de sub invertor - se detecteaza automat la scanare)
#define BRIDGE_LOGGER_MAC ""
#define BRIDGE_LOGGER_IP ""
#define BRIDGE_LOGGER_SN 0UL

// 3. Optional: URL Server extern (nefolosit in modul server web local http://deye.local/)
#define BRIDGE_SERVER_URL ""
#define BRIDGE_API_KEY ""

// 4. Intervalul de citire a datelor din invertor (ms) - implicit 5 secunde
#define BRIDGE_SEND_INTERVAL_MS 5000

// 5. Portal Wi-Fi de configurare locala (AP Mode) & buton BOOT (GPIO 0)
#define BRIDGE_AP_SSID "Deye-ESP32-Setup"
#define BRIDGE_BOOT_BTN_GPIO 0
#define FW_VERSION "v1.0.3"
#define GITHUB_OTA_VERSION_URL "https://raw.githubusercontent.com/petruleonard/deye-smart-energy/main/ota/version.json"
#define GITHUB_OTA_BIN_URL     "https://raw.githubusercontent.com/petruleonard/deye-smart-energy/main/build/deye_esp32_s3_bridge.bin"
