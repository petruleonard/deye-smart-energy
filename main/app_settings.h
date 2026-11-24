#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char logger_mac[20];
    char logger_ip[16];
    uint32_t logger_sn;
    char server_url[192];
    char api_key[65];
    char tg_bot_token[80];
    char tg_chat_id[32];
    char admin_pin[16];
    uint32_t send_interval_ms;
    bool is_configured;
    bool bat_sched_enabled;
    uint16_t bat_pause_start_min;
    uint16_t bat_pause_end_min;
    uint16_t bat_norm_chg_a;
    bool tg_bms_alert_enabled;
    bool tg_grid_alert_enabled;
    bool tg_bat_freeze_alert_enabled;
    int8_t bat_freeze_temp_c;
    uint8_t inv_phases;
    uint16_t inv_dev_type;
    bool wifi_eco_mode;
    char gemini_api_key[128];
    char gemini_model[48];
    char weather_city[48];
    float weather_lat;
    float weather_lon;
    char ac_brand[12];
    char ac_tcl_email[96];
    char ac_tcl_password[64];
    char ac_tuya_region[8];
    char ac_tuya_client_id[48];
    char ac_tuya_client_secret[64];
    char ac_tuya_device_id[48];
    char ac_selected_device_id[48];
    char ac_devices_json[512];
} app_settings_t;

// Global settings instance
extern app_settings_t g_settings;

esp_err_t app_settings_init(void);
esp_err_t app_settings_save(const app_settings_t *settings);

#ifdef __cplusplus
}
#endif
