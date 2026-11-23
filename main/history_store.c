#include "history_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"

static const char *TAG = "history_store";
#define HIST_FILE "/spiffs/history.csv"
#define HIST_TMP  "/spiffs/history.tmp"

static SemaphoreHandle_t s_hist_mutex = NULL;
static bool s_spiffs_ok = false;
static bool s_sntp_started = false;
static int64_t s_last_save_us = 0;
static int64_t s_last_chart_us = 0;

#define MAX_CHART_PTS 288
typedef struct {
    int32_t pv;
    int32_t grid;
    int32_t load;
    uint16_t min_of_day;
    uint8_t soc;
} chart_pt_t;

static chart_pt_t s_chart_pts[MAX_CHART_PTS];
static int s_chart_count = 0;
static int s_chart_day = -1;
static bool s_fast_test = false;

typedef struct {
    char date[12];
    float pv;
    float load;
    float buy;
    float sell;
    float chg;
    float dis;
} hist_row_t;

void history_store_init(void)
{
    if (!s_hist_mutex) {
        s_hist_mutex = xSemaphoreCreateMutex();
    }

    // Romania timezone (EET/EEST)
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        s_spiffs_ok = true;
        ESP_LOGI(TAG, "SPIFFS mounted at /spiffs for local history");
    } else {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
    }
}

void history_store_start_sntp(void)
{
    if (s_sntp_started) return;
    s_sntp_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP time sync started (pool.ntp.org)");
}

void history_store_record(const deye_data_t *d)
{
    if (!d || !d->logger_connected) return;

    int64_t now_us = esp_timer_get_time();

    // Record intraday chart point every 5 minutes (or 5s in fast test mode)
    int64_t interval_us = s_fast_test ? 5000000LL : 300000000LL;
    if (s_last_chart_us == 0 || (now_us - s_last_chart_us) >= interval_us) {
        time_t tnow = 0;
        time(&tnow);
        struct tm tmi;
        localtime_r(&tnow, &tmi);
        if (!s_fast_test && tmi.tm_year + 1900 < 2024) {
            goto skip_chart_until_sntp;
        }
        if (xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!s_fast_test && tmi.tm_year + 1900 >= 2024 && s_chart_day != -1 && s_chart_day != tmi.tm_mday) {
                s_chart_count = 0;
            }
            if (!s_fast_test && tmi.tm_year + 1900 >= 2024) {
                s_chart_day = tmi.tm_mday;
            }
            if (s_fast_test && s_chart_count >= 24) {
                s_chart_count = 0; // Zi noua de la 00:00 in modul rapid 24h
            }
            uint16_t mod = s_fast_test
                ? (uint16_t)(s_chart_count * 60)
                : (uint16_t)(tmi.tm_hour * 60 + tmi.tm_min);
            if (!s_fast_test && s_chart_count > 0 && mod < s_chart_pts[s_chart_count - 1].min_of_day) {
                s_chart_count = 0; // Trecere peste miezul noptii -> grafic nou
            }
            if (s_chart_count >= MAX_CHART_PTS) {
                memmove(&s_chart_pts[0], &s_chart_pts[1], (MAX_CHART_PTS - 1) * sizeof(chart_pt_t));
                s_chart_count = MAX_CHART_PTS - 1;
            }
            s_chart_pts[s_chart_count].pv = d->pv_power_w;
            s_chart_pts[s_chart_count].grid = d->grid_power_w;
            s_chart_pts[s_chart_count].load = d->load_power_w;
            s_chart_pts[s_chart_count].min_of_day = mod;
            s_chart_pts[s_chart_count].soc = (uint8_t)d->battery_soc;
            s_chart_count++;
            s_last_chart_us = now_us;
            xSemaphoreGive(s_hist_mutex);
        }
    }
skip_chart_until_sntp:

    if (!s_spiffs_ok) return;
    if (s_last_save_us != 0 && (now_us - s_last_save_us) < 60000000LL) {
        return; // Save at most once per 60s to protect Flash wear
    }

    time_t now = 0;
    time(&now);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    if (tm_info.tm_year + 1900 < 2024) {
        return; // Wait until SNTP syncs valid date
    }

    char today[12];
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);

    if (xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return;

    FILE *fin = fopen(HIST_FILE, "r");
    FILE *fout = fopen(HIST_TMP, "w");
    if (!fout) {
        if (fin) fclose(fin);
        xSemaphoreGive(s_hist_mutex);
        return;
    }

    bool updated = false;
    char line[128];
    if (fin) {
        while (fgets(line, sizeof(line), fin)) {
            hist_row_t r = {0};
            if (sscanf(line, "%11[^,],%f,%f,%f,%f,%f,%f",
                       r.date, &r.pv, &r.load, &r.buy, &r.sell, &r.chg, &r.dis) == 7) {
                if (strcmp(r.date, today) == 0) {
                    if (d->pv_today_kwh > r.pv) r.pv = d->pv_today_kwh;
                    if (d->load_today_kwh > r.load) r.load = d->load_today_kwh;
                    if (d->grid_buy_today_kwh > r.buy) r.buy = d->grid_buy_today_kwh;
                    if (d->grid_sell_today_kwh > r.sell) r.sell = d->grid_sell_today_kwh;
                    if (d->battery_charge_today_kwh > r.chg) r.chg = d->battery_charge_today_kwh;
                    if (d->battery_discharge_today_kwh > r.dis) r.dis = d->battery_discharge_today_kwh;
                    updated = true;
                }
                fprintf(fout, "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                        r.date, r.pv, r.load, r.buy, r.sell, r.chg, r.dis);
            }
        }
        fclose(fin);
    }

    if (!updated) {
        fprintf(fout, "%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                today, d->pv_today_kwh, d->load_today_kwh,
                d->grid_buy_today_kwh, d->grid_sell_today_kwh,
                d->battery_charge_today_kwh, d->battery_discharge_today_kwh);
    }

    fclose(fout);
    unlink(HIST_FILE);
    rename(HIST_TMP, HIST_FILE);
    s_last_save_us = now_us;
    xSemaphoreGive(s_hist_mutex);
}

void history_store_query(const char *period, const char *date_str, char *out_json, size_t out_sz)
{
    double pv = 0, load = 0, buy = 0, sell = 0, chg = 0, dis = 0;
    bool has_data = false;

    if (s_spiffs_ok && xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        FILE *fin = fopen(HIST_FILE, "r");
        if (fin) {
            char line[128];
            while (fgets(line, sizeof(line), fin)) {
                hist_row_t r = {0};
                if (sscanf(line, "%11[^,],%f,%f,%f,%f,%f,%f",
                           r.date, &r.pv, &r.load, &r.buy, &r.sell, &r.chg, &r.dis) == 7) {
                    bool match = false;
                    if (strcmp(period, "total") == 0) {
                        match = true;
                    } else if (strcmp(period, "year") == 0 && date_str && strlen(date_str) >= 4) {
                        match = (strncmp(r.date, date_str, 4) == 0);
                    } else if (strcmp(period, "month") == 0 && date_str && strlen(date_str) >= 7) {
                        match = (strncmp(r.date, date_str, 7) == 0);
                    } else if (date_str && strlen(date_str) >= 10) {
                        match = (strcmp(r.date, date_str) == 0);
                    }
                    if (match) {
                        has_data = true;
                        pv += r.pv;
                        load += r.load;
                        buy += r.buy;
                        sell += r.sell;
                        chg += r.chg;
                        dis += r.dis;
                    }
                }
            }
            fclose(fin);
        }
        xSemaphoreGive(s_hist_mutex);
    }

    snprintf(out_json, out_sz,
             "{\"has_data\":%s,\"pv_kwh\":%.1f,\"load_kwh\":%.1f,"
             "\"grid_buy_kwh\":%.1f,\"grid_sell_kwh\":%.1f,"
             "\"bat_chg_kwh\":%.1f,\"bat_dis_kwh\":%.1f}",
             has_data ? "true" : "false", pv, load, buy, sell, chg, dis);
}

bool history_store_clear(void)
{
    if (!s_spiffs_ok) return false;
    if (xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    unlink(HIST_FILE);
    s_last_save_us = 0;
    s_chart_count = 0;
    s_last_chart_us = 0;
    xSemaphoreGive(s_hist_mutex);
    return true;
}

void history_store_enable_fast_test(bool enable)
{
    if (!s_hist_mutex || xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
    s_fast_test = enable;
    s_chart_count = 0;
    s_last_chart_us = 0;
    xSemaphoreGive(s_hist_mutex);
    ESP_LOGW(TAG, "Chart fast 24h test mode: %s", enable ? "ON" : "OFF");
}

void history_store_chart_json(char *out_json, size_t out_sz)
{
    if (!out_json || out_sz < 32) return;
    if (!s_hist_mutex || xSemaphoreTake(s_hist_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        snprintf(out_json, out_sz, "{\"points\":[]}");
        return;
    }

    size_t pos = snprintf(out_json, out_sz, "{\"points\":[");
    int step = (s_chart_count > 96) ? (s_chart_count / 96) : 1;
    bool first = true;
    for (int i = 0; i < s_chart_count && pos + 80 < out_sz; i += step) {
        pos += snprintf(out_json + pos, out_sz - pos,
                        "%s{\"m\":%u,\"pv\":%ld,\"grid\":%ld,\"soc\":%u,\"load\":%ld}",
                        first ? "" : ",",
                        (unsigned)s_chart_pts[i].min_of_day,
                        (long)s_chart_pts[i].pv,
                        (long)s_chart_pts[i].grid,
                        (unsigned)s_chart_pts[i].soc,
                        (long)s_chart_pts[i].load);
        first = false;
    }
    snprintf(out_json + pos, out_sz - pos, "]}");
    xSemaphoreGive(s_hist_mutex);
}

bool history_store_get_storage_info(size_t *total_bytes, size_t *used_bytes)
{
    if (!s_spiffs_ok || !total_bytes || !used_bytes) return false;
    return (esp_spiffs_info("storage", total_bytes, used_bytes) == ESP_OK);
}
