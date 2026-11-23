#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init(void);
void wifi_manager_connect(void);
void wifi_manager_prepare_reboot(void);
bool wifi_manager_is_connected(void);
const char* wifi_manager_get_ip(void);
int wifi_manager_get_rssi(void);
void wifi_manager_apply_power_mode(void);
int wifi_manager_get_tx_dbm(void);
int wifi_manager_get_cpu_mhz(void);

esp_err_t wifi_manager_start_ap_portal(void);
esp_err_t wifi_manager_start_fallback_ap(void);
void wifi_manager_stop_ap_portal(void);
bool wifi_manager_is_ap_active(void);
bool wifi_manager_test_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// Scans available Wi-Fi networks (blocking up to ~3 seconds)
esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records, uint16_t *max_ap_count);

#ifdef __cplusplus
}
#endif
