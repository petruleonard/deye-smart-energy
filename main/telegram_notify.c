#include "telegram_notify.h"
#include "app_settings.h"
#include "wifi_manager.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "esp_heap_caps.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "telegram";

static int s_outage_streak = 0;
static bool s_outage_alert_sent = false;

static void json_escape_text(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_sz; i++) {
        char c = src[i];
        if (c == '\"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if ((unsigned char)c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

esp_err_t telegram_notify_send(const char *text)
{
    if (!text || text[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (g_settings.tg_bot_token[0] == '\0' || g_settings.tg_chat_id[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_manager_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", g_settings.tg_bot_token);

    size_t esc_sz = 4096;
    size_t body_sz = 4256;
    char *esc_text = heap_caps_calloc(1, esc_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!esc_text) esc_text = calloc(1, esc_sz);
    char *body = heap_caps_calloc(1, body_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = calloc(1, body_sz);
    if (!esc_text || !body) {
        free(esc_text);
        free(body);
        return ESP_ERR_NO_MEM;
    }

    json_escape_text(text, esc_text, esc_sz);
    snprintf(body, body_sz, "{\"chat_id\":\"%s\",\"text\":\"%s\"}",
             g_settings.tg_chat_id, esc_text);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(esc_text);
        free(body);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(esc_text);
    free(body);

    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Telegram send failed: err=%s status=%d", esp_err_to_name(err), status);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    ESP_LOGI(TAG, "Mesaj Telegram trimis cu succes (status=%d)", status);
    return ESP_OK;
}

esp_err_t telegram_notify_send_test(void)
{
    return telegram_notify_send("Deye Monitor ESP32-S3: Notificarile pe Telegram functioneaza!");
}

void telegram_notify_check_grid(const deye_data_t *d)
{
    if (!d || !d->logger_connected) return;
    if (g_settings.tg_bot_token[0] == '\0' || g_settings.tg_chat_id[0] == '\0') return;

    if (!g_settings.tg_grid_alert_enabled) {
        s_outage_streak = 0;
        s_outage_alert_sent = false;
    } else if (d->grid_volts < 50.0f) {
        s_outage_streak++;
        if (s_outage_streak >= 2 && !s_outage_alert_sent) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "AVARIE RETEA! Tensiunea din retea a disparut (L1: %.0f V).\n"
                     "Baterie: %u%% | Consum casa: %ld W",
                     d->grid_volts, (unsigned)d->battery_soc, (long)d->load_power_w);
            if (telegram_notify_send(msg) == ESP_OK) {
                s_outage_alert_sent = true;
            }
        }
    } else if (d->grid_volts >= 180.0f) {
        s_outage_streak = 0;
        if (s_outage_alert_sent) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "RETEA REVENITA! Tensiunea a revenit la %.0f V.\n"
                     "Baterie: %u%% | Consum casa: %ld W",
                     d->grid_volts, (unsigned)d->battery_soc, (long)d->load_power_w);
            if (telegram_notify_send(msg) == ESP_OK) {
                s_outage_alert_sent = false;
            }
        }
    }

    // Check BMS communication loss if enabled
    static int s_bms_lost_streak = 0;
    static bool s_bms_alert_sent = false;
    if (!g_settings.tg_bms_alert_enabled) {
        s_bms_lost_streak = 0;
        s_bms_alert_sent = false;
    } else {
        bool bms_lost = (d->battery_soc == 0 && (d->battery_soh == 0 || d->battery_volts < 10.0f));
        if (bms_lost) {
            s_bms_lost_streak++;
            if (s_bms_lost_streak >= 6 && !s_bms_alert_sent) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "ALERTA BATERIE (BMS)! S-a pierdut comunicatia BMS cu bateria (SoC: %u%%, Tensiune: %.1f V).",
                         (unsigned)d->battery_soc, d->battery_volts);
                if (telegram_notify_send(msg) == ESP_OK) {
                    s_bms_alert_sent = true;
                }
            }
        } else if (d->battery_soc > 0 && d->battery_volts >= 10.0f) {
            s_bms_lost_streak = 0;
            if (s_bms_alert_sent) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "BATERIE OK: Comunicatia BMS cu bateria a fost restabilita (SoC: %u%%, %.1f V).",
                         (unsigned)d->battery_soc, d->battery_volts);
                if (telegram_notify_send(msg) == ESP_OK) {
                    s_bms_alert_sent = false;
                }
            }
        }
    }

    // Check battery low temperature / freeze risk alert (only triggers on drop, never on rise)
    static int s_freeze_streak = 0;
    static int s_freeze_last_alerted_c = 127;
    if (!g_settings.tg_bat_freeze_alert_enabled) {
        s_freeze_streak = 0;
        s_freeze_last_alerted_c = 127;
        return;
    }

    if (d->battery_volts >= 10.0f && d->battery_temp_c > -40.0f && d->battery_temp_c < 80.0f) {
        int temp_floor = (int)floorf(d->battery_temp_c + 0.001f);
        int thresh = (int)g_settings.bat_freeze_temp_c;
        if (temp_floor <= thresh) {
            s_freeze_streak++;
            if (s_freeze_streak >= 2) {
                if (s_freeze_last_alerted_c == 127 || temp_floor < s_freeze_last_alerted_c) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "ALERTA RISC INGHET BATERIE! Temperatura scazuta: %.1f C (prag setat: %d C). SoC: %u%%",
                             d->battery_temp_c, thresh, (unsigned)d->battery_soc);
                    if (telegram_notify_send(msg) == ESP_OK) {
                        s_freeze_last_alerted_c = temp_floor;
                    }
                }
            }
        } else {
            s_freeze_streak = 0;
            if (temp_floor >= thresh + 2) {
                s_freeze_last_alerted_c = 127;
            }
        }
    }
}
