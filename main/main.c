#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "app_config.h"
#include "app_settings.h"
#include "deye_data.h"
#include "deye_modbus.h"
#include "history_store.h"
#include "local_web_server.h"
#include "solarman_v5.h"
#include "sys_metrics.h"
#include "telegram_notify.h"
#include "wifi_manager.h"

static const char *TAG = "main";

#define BOOT_BTN_GPIO GPIO_NUM_0

static void start_mdns_service(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init esuat: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("deye");
    mdns_instance_name_set("Deye Solar Monitor");
    mdns_service_add("Deye Web", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS pornit: http://deye.local/");
}

static void boot_button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    int held_ms = 0;
    while (1) {
        if (gpio_get_level(BOOT_BTN_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= 3000) {
                ESP_LOGW(TAG, "Buton BOOT tinut 3s -> deschid portalul Wi-Fi Setup!");
                wifi_manager_start_ap_portal();
                held_ms = 0;
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=== Deye ESP32-S3 Local Web Server ===");

    app_settings_init();
    deye_data_init();
    wifi_manager_init();

    solarman_v5_init();
    sys_metrics_init();
    history_store_init();

    if (!g_settings.is_configured || g_settings.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Neconfigurat -> pornesc automat portalul Wi-Fi Setup!");
        wifi_manager_start_ap_portal();
    }

    start_mdns_service();
    esp_err_t web_err = local_web_server_init();
    deye_modbus_init();

    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);

    bool ota_pending_verify = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ota_pending_verify = true;
        ESP_LOGW(TAG, "Firmware nou OTA in stare PENDING_VERIFY -> verific sanatatea...");
        if (web_err != ESP_OK) {
            ESP_LOGE(TAG, "Server web esuat -> ROLLBACK la versiunea anterioara!");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        if (!g_settings.is_configured || g_settings.wifi_ssid[0] == '\0') {
            ESP_LOGI(TAG, "Neconfigurat, dar portal + server OK -> confirm OTA valid.");
            esp_ota_mark_app_valid_cancel_rollback();
            ota_pending_verify = false;
        }
    }

    bool sntp_started = false;
    bool auto_ap_opened = false;
    int wait_wifi_ms = 0;
    int last_ota_check_yday = -1;
    while (1) {
        if (!sntp_started && wifi_manager_is_connected()) {
            history_store_start_sntp();
            sntp_started = true;
            wifi_manager_apply_power_mode();
            if (auto_ap_opened && g_settings.is_configured) {
                wifi_manager_stop_ap_portal();
                auto_ap_opened = false;
            }
            if (ota_pending_verify) {
                ESP_LOGI(TAG, "Wi-Fi conectat + server web OK -> confirm OTA valid!");
                esp_ota_mark_app_valid_cancel_rollback();
                ota_pending_verify = false;
            }
        } else if (ota_pending_verify && !wifi_manager_is_connected() && wait_wifi_ms >= 45000) {
            ESP_LOGE(TAG, "Wi-Fi nu s-a conectat in 45s dupa OTA -> ROLLBACK automat!");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        } else if (!wifi_manager_is_connected() && !wifi_manager_is_ap_active() && wait_wifi_ms >= 30000) {
            ESP_LOGW(TAG, "Wi-Fi nu s-a conectat in 30s -> pornesc automat portalul Wi-Fi Setup (fallback)!");
            auto_ap_opened = true;
            wifi_manager_start_fallback_ap();
        }

        if (sntp_started && wifi_manager_is_connected()) {
            time_t now = 0;
            struct tm tmi = {0};
            time(&now);
            localtime_r(&now, &tmi);
            if ((tmi.tm_year + 1900) >= 2024 && tmi.tm_hour == 18 && tmi.tm_min < 5 && last_ota_check_yday != tmi.tm_yday) {
                last_ota_check_yday = tmi.tm_yday;
                extern void ota_daily_check_task(void *arg);
                xTaskCreate(ota_daily_check_task, "ota_daily", 6144, NULL, 4, NULL);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
        wait_wifi_ms += 2000;
        if ((wait_wifi_ms % 16000) == 0 && wifi_manager_is_connected()) {
            wifi_manager_apply_power_mode();
        }
    }
}

void ota_daily_check_task(void *arg)
{
    (void)arg;
    char latest[32] = "";
    char url[256] = "";
    char notes[160] = "";
    if (github_ota_check_latest(latest, sizeof(latest), url, sizeof(url), notes, sizeof(notes))) {
        if (strcmp(latest, FW_VERSION) != 0) {
            char msg[360];
            snprintf(msg, sizeof(msg),
                     "ACTUALIZARE DISPONIBILA (%s)!\nVersiune curenta: %s\nNoutati: %s\nDeschide Setari -> Verifica Actualizari pe GitHub pentru instalare.",
                     latest, FW_VERSION, notes[0] ? notes : "Imbunatatiri si corectii");
            telegram_notify_send(msg);
        }
    }
    vTaskDelete(NULL);
}
