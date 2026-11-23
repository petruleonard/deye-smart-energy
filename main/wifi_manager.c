#include "wifi_manager.h"
#include "app_config.h"
#include "app_settings.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif
#include <string.h>

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static bool s_is_connected = false;
static bool s_ap_active = false;
static bool s_fallback_ap = false;
static bool s_rebooting = false;
static volatile bool s_pause_reconnect = false;
static int s_retry_count = 0;
static esp_timer_handle_t s_reconn_timer = NULL;
static char s_ip_str[16] = "0.0.0.0";
static int s_cur_tx_power_qdbm = 80;
static int s_cur_cpu_max_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
static bool s_pm_eco_applied = false;
static bool s_pm_initialized = false;

static void apply_cpu_pm_config(bool eco) {
#ifdef CONFIG_PM_ENABLE
  if (s_pm_initialized && s_pm_eco_applied == eco) {
    return;
  }
  esp_pm_config_t pm = {
      .max_freq_mhz = eco ? 160 : 240,
      .min_freq_mhz = eco ? 80 : 240,
      .light_sleep_enable = false,
  };
  esp_err_t err = esp_pm_configure(&pm);
  if (err == ESP_OK) {
    s_cur_cpu_max_mhz = pm.max_freq_mhz;
    s_pm_eco_applied = eco;
    s_pm_initialized = true;
    ESP_LOGI(TAG, "CPU Power Mode: %s (%d-%d MHz)",
             eco ? "Mod Eco" : "Normal", pm.min_freq_mhz, pm.max_freq_mhz);
  } else {
    ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(err));
  }
#else
  (void)eco;
#endif
}

static void reconnect_timer_cb(void *arg) {
  (void)arg;
  if (!s_rebooting && !s_pause_reconnect && !s_is_connected && strlen(g_settings.wifi_ssid) > 0) {
    esp_wifi_connect();
  }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // Connection is triggered explicitly by wifi_manager_connect() after
    // setting config.
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *dis =
        (wifi_event_sta_disconnected_t *)event_data;
    uint8_t reason = dis ? dis->reason : 0;
    s_is_connected = false;
    strcpy(s_ip_str, "0.0.0.0");
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(80);
    if (!s_rebooting && !s_pause_reconnect && strlen(g_settings.wifi_ssid) > 0) {
      s_retry_count++;
      uint64_t delay_us =
          (s_retry_count <= 5)
              ? 300000ULL
              : ((s_retry_count <= 15) ? 1500000ULL : 5000000ULL);
      ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%u). Reconnecting in %lums...",
               (unsigned)reason, (unsigned long)(delay_us / 1000ULL));
      if (s_reconn_timer) {
        esp_timer_stop(s_reconn_timer);
        esp_timer_start_once(s_reconn_timer, delay_us);
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Wi-Fi connected! Got IP: %s", s_ip_str);
    s_is_connected = true;
    s_retry_count = 0;
    if (s_reconn_timer)
      esp_timer_stop(s_reconn_timer);
    if (s_fallback_ap && g_settings.is_configured) {
      wifi_manager_stop_ap_portal();
    }
    wifi_manager_apply_power_mode();
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

esp_err_t wifi_manager_init(void) {
  if (s_wifi_event_group == NULL) {
    s_wifi_event_group = xEventGroupCreate();
    const esp_timer_create_args_t targs = {.callback = &reconnect_timer_cb,
                                           .name = "wifi_reconn"};
    esp_timer_create(&targs, &s_reconn_timer);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    (void)s_sta_netif;
    (void)s_ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    apply_cpu_pm_config(g_settings.wifi_eco_mode);
  }

  wifi_manager_connect();
  return ESP_OK;
}

void wifi_manager_connect(void) {
  if (strlen(g_settings.wifi_ssid) == 0) {
    ESP_LOGW(TAG, "Wi-Fi SSID is empty, skipping STA connection.");
    return;
  }

  wifi_config_t cur_cfg = {0};
  bool need_set = true;
  if (esp_wifi_get_config(WIFI_IF_STA, &cur_cfg) == ESP_OK) {
    if (strcmp((const char *)cur_cfg.sta.ssid, g_settings.wifi_ssid) == 0 &&
        strcmp((const char *)cur_cfg.sta.password, g_settings.wifi_password) ==
            0 &&
        !cur_cfg.sta.pmf_cfg.capable) {
      need_set =
          false; // Preserve ESP-IDF NVS fast-connect channel & BSSID cache
    }
  }

  if (need_set) {
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, g_settings.wifi_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, g_settings.wifi_password,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (strlen(g_settings.wifi_password) > 0)
                                             ? WIFI_AUTH_WPA2_PSK
                                             : WIFI_AUTH_OPEN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  }

  s_retry_count = 0;
  ESP_LOGI(TAG, "Connecting to Wi-Fi SSID: %s (%s)", g_settings.wifi_ssid,
           need_set ? "new config" : "fast NVS cache");
  esp_wifi_connect();
}

void wifi_manager_stop_ap_portal(void) {
  s_fallback_ap = false;
  if (!s_ap_active)
    return;
  s_ap_active = false;
  esp_wifi_set_mode(WIFI_MODE_STA);
  ESP_LOGI(TAG, "Wi-Fi fallback SoftAP stopped -> full STA mode");
}

void wifi_manager_prepare_reboot(void) {
  s_rebooting = true;
  if (s_reconn_timer)
    esp_timer_stop(s_reconn_timer);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(150));
  esp_wifi_stop();
}

static esp_err_t start_ap_internal(bool is_fallback) {
  s_fallback_ap = is_fallback;
  if (s_ap_active) {
    return ESP_OK;
  }
  s_ap_active = true;
  esp_wifi_disconnect();

  wifi_config_t ap_config = {0};
  strncpy((char *)ap_config.ap.ssid, BRIDGE_AP_SSID,
          sizeof(ap_config.ap.ssid) - 1);
  ap_config.ap.ssid_len = strlen(BRIDGE_AP_SSID);
  ap_config.ap.channel = 6;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  if (strlen(g_settings.wifi_ssid) > 0) {
    esp_wifi_connect();
  }
  ESP_LOGI(
      TAG,
      "Wi-Fi Config Portal SoftAP started%s: SSID='%s' -> http://192.168.4.1",
      is_fallback ? " (fallback)" : "", BRIDGE_AP_SSID);
  return ESP_OK;
}

esp_err_t wifi_manager_start_ap_portal(void) {
  return start_ap_internal(false);
}

esp_err_t wifi_manager_start_fallback_ap(void) {
  return start_ap_internal(true);
}

bool wifi_manager_is_ap_active(void) { return s_ap_active; }

bool wifi_manager_test_connect(const char *ssid, const char *password,
                               uint32_t timeout_ms) {
  if (!ssid || strlen(ssid) == 0)
    return false;

  s_is_connected = false;
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
  if (password) {
    strncpy((char *)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);
  }
  wifi_config.sta.threshold.authmode =
      (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

  ESP_LOGI(TAG, "Testing STA Wi-Fi connection to '%s' while keeping AP open...",
           ssid);
  s_pause_reconnect = true;
  if (s_reconn_timer) esp_timer_stop(s_reconn_timer);
  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(150));
  if (esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
    s_pause_reconnect = false;
    return false;
  }
  s_pause_reconnect = false;
  esp_wifi_connect();

  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE,
                          pdTRUE, pdMS_TO_TICKS(timeout_ms));
  return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_manager_is_connected(void) { return s_is_connected; }

const char *wifi_manager_get_ip(void) { return s_ip_str; }

esp_err_t wifi_manager_scan(wifi_ap_record_t *ap_records,
                            uint16_t *max_ap_count) {
  if (!ap_records || !max_ap_count || *max_ap_count == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  for (int w = 0; w < 40 && s_pause_reconnect; w++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  s_pause_reconnect = true;
  if (s_reconn_timer) esp_timer_stop(s_reconn_timer);
  if (!s_is_connected) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
  }

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      .scan_time.active.min = 60,
      .scan_time.active.max = 180,
  };

  ESP_LOGI(TAG, "Starting Wi-Fi scan...");
  esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
  if (ret == ESP_ERR_WIFI_STATE) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(250));
    ret = esp_wifi_scan_start(&scan_config, true);
  }
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
    s_pause_reconnect = false;
    if (!s_is_connected && strlen(g_settings.wifi_ssid) > 0 && s_reconn_timer) {
      esp_timer_start_once(s_reconn_timer, 1500000ULL);
    }
    return ret;
  }

  ret = esp_wifi_scan_get_ap_records(max_ap_count, ap_records);
  ESP_LOGI(TAG, "Wi-Fi scan finished, found %u APs", (unsigned)*max_ap_count);
  s_pause_reconnect = false;
  if (!s_is_connected && strlen(g_settings.wifi_ssid) > 0 && s_reconn_timer) {
    esp_timer_start_once(s_reconn_timer, 1500000ULL);
  }
  return ret;
}

int wifi_manager_get_rssi(void) {
  if (!wifi_manager_is_connected())
    return 0;
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
    return (int)ap_info.rssi;
  }
  return 0;
}

int wifi_manager_get_tx_dbm(void) {
  int8_t p = (int8_t)s_cur_tx_power_qdbm;
  if (esp_wifi_get_max_tx_power(&p) == ESP_OK) {
    s_cur_tx_power_qdbm = (int)p;
  }
  return (s_cur_tx_power_qdbm + 2) / 4;
}

int wifi_manager_get_cpu_mhz(void) { return s_cur_cpu_max_mhz; }

void wifi_manager_apply_power_mode(void) {
  apply_cpu_pm_config(g_settings.wifi_eco_mode);

  if (!g_settings.wifi_eco_mode || !wifi_manager_is_connected()) {
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (s_cur_tx_power_qdbm != 80) {
      if (esp_wifi_set_max_tx_power(80) == ESP_OK) {
        s_cur_tx_power_qdbm = 80;
        ESP_LOGI(TAG, "Wi-Fi Normal: WIFI_PS_NONE, TX=20 dBm");
      }
    }
    return;
  }

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  int rssi = wifi_manager_get_rssi();
  if (rssi >= 0 || rssi < -100) {
    return;
  }

  int target_qdbm = 80; // 20 dBm
  if (rssi >= -52) {
    target_qdbm = 34;   // ~8.5 dBm (~7 mW)
  } else if (rssi >= -60) {
    target_qdbm = 44;   // 11 dBm (~12.5 mW)
  } else if (rssi >= -68) {
    target_qdbm = 56;   // 14 dBm (~25 mW)
  } else if (rssi >= -75) {
    target_qdbm = 66;   // 16.5 dBm (~45 mW)
  }

  if (target_qdbm != s_cur_tx_power_qdbm) {
    if (esp_wifi_set_max_tx_power((int8_t)target_qdbm) == ESP_OK) {
      s_cur_tx_power_qdbm = target_qdbm;
      ESP_LOGI(TAG, "Wi-Fi Mod Eco: WIFI_PS_MIN_MODEM, RSSI=%d dBm -> TX=%d dBm",
               rssi, (target_qdbm + 2) / 4);
    }
  }
}
