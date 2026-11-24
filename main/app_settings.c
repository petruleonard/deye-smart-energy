#include "app_settings.h"
#include "app_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "app_settings";
#define SETTINGS_NAMESPACE "deye_cfg"

app_settings_t g_settings = {0};

static void load_defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    strncpy(g_settings.wifi_ssid, BRIDGE_WIFI_SSID, sizeof(g_settings.wifi_ssid) - 1);
    strncpy(g_settings.wifi_password, BRIDGE_WIFI_PASSWORD, sizeof(g_settings.wifi_password) - 1);
    strncpy(g_settings.logger_mac, BRIDGE_LOGGER_MAC, sizeof(g_settings.logger_mac) - 1);
    strncpy(g_settings.logger_ip, BRIDGE_LOGGER_IP, sizeof(g_settings.logger_ip) - 1);
    g_settings.logger_sn = BRIDGE_LOGGER_SN;
    strncpy(g_settings.server_url, BRIDGE_SERVER_URL, sizeof(g_settings.server_url) - 1);
    strncpy(g_settings.api_key, BRIDGE_API_KEY, sizeof(g_settings.api_key) - 1);
    g_settings.send_interval_ms = BRIDGE_SEND_INTERVAL_MS;
    g_settings.is_configured = (strlen(g_settings.wifi_ssid) > 0);
    g_settings.bat_sched_enabled = false;
    g_settings.bat_pause_start_min = 360;
    g_settings.bat_pause_end_min = 660;
    g_settings.bat_norm_chg_a = 0;
    g_settings.tg_bms_alert_enabled = false;
    g_settings.tg_grid_alert_enabled = true;
    g_settings.tg_bat_freeze_alert_enabled = false;
    g_settings.bat_freeze_temp_c = 8;
    g_settings.gemini_api_key[0] = '\0';
    strncpy(g_settings.gemini_model, "gemini-flash-lite-latest", sizeof(g_settings.gemini_model) - 1);
}

esp_err_t app_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS: %s", esp_err_to_name(err));
        return err;
    }

    load_defaults();

    nvs_handle_t nvs;
    err = nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved settings found in NVS (using defaults)");
        return ESP_OK;
    }

    size_t len = sizeof(g_settings.wifi_ssid);
    nvs_get_str(nvs, "wifi_ssid", g_settings.wifi_ssid, &len);

    len = sizeof(g_settings.wifi_password);
    nvs_get_str(nvs, "wifi_pass", g_settings.wifi_password, &len);

    len = sizeof(g_settings.logger_mac);
    if (nvs_get_str(nvs, "logger_mac", g_settings.logger_mac, &len) != ESP_OK || strlen(g_settings.logger_mac) == 0) {
        strncpy(g_settings.logger_mac, BRIDGE_LOGGER_MAC, sizeof(g_settings.logger_mac) - 1);
    }

    len = sizeof(g_settings.logger_ip);
    nvs_get_str(nvs, "logger_ip", g_settings.logger_ip, &len);

    nvs_get_u32(nvs, "logger_sn", &g_settings.logger_sn);

    len = sizeof(g_settings.server_url);
    if (nvs_get_str(nvs, "server_url", g_settings.server_url, &len) != ESP_OK || strlen(g_settings.server_url) == 0) {
        strncpy(g_settings.server_url, BRIDGE_SERVER_URL, sizeof(g_settings.server_url) - 1);
    }

    len = sizeof(g_settings.api_key);
    if (nvs_get_str(nvs, "api_key", g_settings.api_key, &len) != ESP_OK) {
        strncpy(g_settings.api_key, BRIDGE_API_KEY, sizeof(g_settings.api_key) - 1);
    }

    uint32_t interval_ms = BRIDGE_SEND_INTERVAL_MS;
    if (nvs_get_u32(nvs, "interval_ms", &interval_ms) == ESP_OK && interval_ms >= 5000 && interval_ms <= 60000) {
        g_settings.send_interval_ms = interval_ms;
    }

    len = sizeof(g_settings.tg_bot_token);
    nvs_get_str(nvs, "tg_token", g_settings.tg_bot_token, &len);

    len = sizeof(g_settings.tg_chat_id);
    nvs_get_str(nvs, "tg_chat", g_settings.tg_chat_id, &len);

    len = sizeof(g_settings.admin_pin);
    nvs_get_str(nvs, "admin_pin", g_settings.admin_pin, &len);

    uint8_t cfg = 0;
    nvs_get_u8(nvs, "configured", &cfg);
    g_settings.is_configured = (cfg == 1 && strlen(g_settings.wifi_ssid) > 0);

    uint8_t u8v = 0;
    if (nvs_get_u8(nvs, "bat_sched", &u8v) == ESP_OK) g_settings.bat_sched_enabled = (u8v == 1);
    nvs_get_u16(nvs, "bat_pstart", &g_settings.bat_pause_start_min);
    nvs_get_u16(nvs, "bat_pend", &g_settings.bat_pause_end_min);
    nvs_get_u16(nvs, "bat_norm_a", &g_settings.bat_norm_chg_a);
    if (nvs_get_u8(nvs, "tg_bms", &u8v) == ESP_OK) g_settings.tg_bms_alert_enabled = (u8v == 1);
    if (nvs_get_u8(nvs, "tg_grid", &u8v) == ESP_OK) g_settings.tg_grid_alert_enabled = (u8v == 1);
    if (nvs_get_u8(nvs, "tg_bat_frz", &u8v) == ESP_OK) g_settings.tg_bat_freeze_alert_enabled = (u8v == 1);
    int8_t i8v = 8;
    if (nvs_get_i8(nvs, "bat_frz_t", &i8v) == ESP_OK) g_settings.bat_freeze_temp_c = i8v;
    if (nvs_get_u8(nvs, "inv_ph", &u8v) == ESP_OK) g_settings.inv_phases = u8v;
    nvs_get_u16(nvs, "inv_dt", &g_settings.inv_dev_type);
    if (nvs_get_u8(nvs, "wifi_eco", &u8v) == ESP_OK) g_settings.wifi_eco_mode = (u8v == 1);

    len = sizeof(g_settings.gemini_api_key);
    nvs_get_str(nvs, "gem_key", g_settings.gemini_api_key, &len);

    len = sizeof(g_settings.gemini_model);
    if (nvs_get_str(nvs, "gem_model", g_settings.gemini_model, &len) != ESP_OK || strlen(g_settings.gemini_model) == 0) {
        strncpy(g_settings.gemini_model, "gemini-flash-lite-latest", sizeof(g_settings.gemini_model) - 1);
    }

    len = sizeof(g_settings.weather_city);
    nvs_get_str(nvs, "w_city", g_settings.weather_city, &len);
    int32_t i32v = 0;
    if (nvs_get_i32(nvs, "w_lat_e4", &i32v) == ESP_OK) g_settings.weather_lat = (float)i32v / 10000.0f;
    if (nvs_get_i32(nvs, "w_lon_e4", &i32v) == ESP_OK) g_settings.weather_lon = (float)i32v / 10000.0f;

    len = sizeof(g_settings.ac_brand);
    nvs_get_str(nvs, "ac_brand", g_settings.ac_brand, &len);
    len = sizeof(g_settings.ac_tcl_email);
    nvs_get_str(nvs, "ac_tcl_em", g_settings.ac_tcl_email, &len);
    len = sizeof(g_settings.ac_tcl_password);
    nvs_get_str(nvs, "ac_tcl_pw", g_settings.ac_tcl_password, &len);
    len = sizeof(g_settings.ac_tuya_region);
    if (nvs_get_str(nvs, "ac_tuya_rg", g_settings.ac_tuya_region, &len) != ESP_OK || strlen(g_settings.ac_tuya_region) == 0) {
        strncpy(g_settings.ac_tuya_region, "eu", sizeof(g_settings.ac_tuya_region) - 1);
    }
    len = sizeof(g_settings.ac_tuya_client_id);
    nvs_get_str(nvs, "ac_tuya_id", g_settings.ac_tuya_client_id, &len);
    len = sizeof(g_settings.ac_tuya_client_secret);
    nvs_get_str(nvs, "ac_tuya_sc", g_settings.ac_tuya_client_secret, &len);
    len = sizeof(g_settings.ac_tuya_device_id);
    nvs_get_str(nvs, "ac_tuya_dv", g_settings.ac_tuya_device_id, &len);
    len = sizeof(g_settings.ac_selected_device_id);
    nvs_get_str(nvs, "ac_sel_id", g_settings.ac_selected_device_id, &len);
    len = sizeof(g_settings.ac_devices_json);
    if (nvs_get_str(nvs, "ac_devs", g_settings.ac_devices_json, &len) != ESP_OK || strlen(g_settings.ac_devices_json) == 0) {
        strncpy(g_settings.ac_devices_json, "[]", sizeof(g_settings.ac_devices_json) - 1);
    }

    nvs_close(nvs);

    ESP_LOGI(TAG, "Settings loaded: SSID='%s', Logger='%s' (%lu), URL='%s', Configured=%d",
             g_settings.wifi_ssid, g_settings.logger_ip, (unsigned long)g_settings.logger_sn,
             g_settings.server_url, g_settings.is_configured);
    return ESP_OK;
}

esp_err_t app_settings_save(const app_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(nvs, "wifi_ssid", settings->wifi_ssid);
    nvs_set_str(nvs, "wifi_pass", settings->wifi_password);
    nvs_set_str(nvs, "logger_mac", settings->logger_mac);
    nvs_set_str(nvs, "logger_ip", settings->logger_ip);
    nvs_set_u32(nvs, "logger_sn", settings->logger_sn);
    nvs_set_str(nvs, "server_url", settings->server_url);
    nvs_set_str(nvs, "api_key", settings->api_key);
    nvs_set_u32(nvs, "interval_ms", settings->send_interval_ms);
    nvs_set_str(nvs, "tg_token", settings->tg_bot_token);
    nvs_set_str(nvs, "tg_chat", settings->tg_chat_id);
    nvs_set_str(nvs, "admin_pin", settings->admin_pin);
    nvs_set_u8(nvs, "configured", (strlen(settings->wifi_ssid) > 0) ? 1 : 0);
    nvs_set_u8(nvs, "bat_sched", settings->bat_sched_enabled ? 1 : 0);
    nvs_set_u16(nvs, "bat_pstart", settings->bat_pause_start_min);
    nvs_set_u16(nvs, "bat_pend", settings->bat_pause_end_min);
    nvs_set_u16(nvs, "bat_norm_a", settings->bat_norm_chg_a);
    nvs_set_u8(nvs, "tg_bms", settings->tg_bms_alert_enabled ? 1 : 0);
    nvs_set_u8(nvs, "tg_grid", settings->tg_grid_alert_enabled ? 1 : 0);
    nvs_set_u8(nvs, "tg_bat_frz", settings->tg_bat_freeze_alert_enabled ? 1 : 0);
    nvs_set_i8(nvs, "bat_frz_t", settings->bat_freeze_temp_c);
    nvs_set_u8(nvs, "inv_ph", settings->inv_phases);
    nvs_set_u16(nvs, "inv_dt", settings->inv_dev_type);
    nvs_set_u8(nvs, "wifi_eco", settings->wifi_eco_mode ? 1 : 0);
    nvs_set_str(nvs, "gem_key", settings->gemini_api_key);
    nvs_set_str(nvs, "gem_model", settings->gemini_model[0] ? settings->gemini_model : "gemini-flash-lite-latest");
    nvs_set_str(nvs, "w_city", settings->weather_city);
    nvs_set_i32(nvs, "w_lat_e4", (int32_t)(settings->weather_lat * 10000.0f));
    nvs_set_i32(nvs, "w_lon_e4", (int32_t)(settings->weather_lon * 10000.0f));
    nvs_set_str(nvs, "ac_brand", settings->ac_brand);
    nvs_set_str(nvs, "ac_tcl_em", settings->ac_tcl_email);
    nvs_set_str(nvs, "ac_tcl_pw", settings->ac_tcl_password);
    nvs_set_str(nvs, "ac_tuya_rg", settings->ac_tuya_region[0] ? settings->ac_tuya_region : "eu");
    nvs_set_str(nvs, "ac_tuya_id", settings->ac_tuya_client_id);
    nvs_set_str(nvs, "ac_tuya_sc", settings->ac_tuya_client_secret);
    nvs_set_str(nvs, "ac_tuya_dv", settings->ac_tuya_device_id);
    nvs_set_str(nvs, "ac_sel_id", settings->ac_selected_device_id);
    nvs_set_str(nvs, "ac_devs", settings->ac_devices_json[0] ? settings->ac_devices_json : "[]");

    err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        memcpy(&g_settings, settings, sizeof(app_settings_t));
        g_settings.is_configured = (strlen(g_settings.wifi_ssid) > 0);
        ESP_LOGI(TAG, "Settings successfully saved to NVS!");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS settings: %s", esp_err_to_name(err));
    }
    return err;
}
