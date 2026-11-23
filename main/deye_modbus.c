#include "deye_modbus.h"
#include "history_store.h"
#include "telegram_notify.h"
#include "solarman_v5.h"
#include "logger_discovery.h"
#include "deye_data.h"
#include "app_settings.h"
#include "wifi_manager.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "deye_modbus";
static volatile bool s_scan_paused = false;

void deye_modbus_set_scan_paused(bool paused)
{
    s_scan_paused = paused;
}

bool deye_modbus_is_scan_paused(void)
{
    return s_scan_paused;
}

static void try_rediscover_logger(void)
{
    char found_ip[16] = {0};
    char found_mac[20] = {0};
    uint32_t found_sn = 0;
    if (logger_discover_by_mac(g_settings.logger_mac, g_settings.logger_sn,
                               found_ip, sizeof(found_ip),
                               found_mac, sizeof(found_mac),
                               &found_sn)) {
        bool changed = (strcmp(found_ip, g_settings.logger_ip) != 0) ||
                       (found_sn > 0 && found_sn != g_settings.logger_sn) ||
                       (found_mac[0] != '\0' && strcmp(found_mac, g_settings.logger_mac) != 0);
        if (changed) {
            app_settings_t next = g_settings;
            strncpy(next.logger_ip, found_ip, sizeof(next.logger_ip) - 1);
            next.logger_ip[sizeof(next.logger_ip) - 1] = '\0';
            if (found_mac[0] != '\0') {
                strncpy(next.logger_mac, found_mac, sizeof(next.logger_mac) - 1);
                next.logger_mac[sizeof(next.logger_mac) - 1] = '\0';
            }
            if (found_sn > 0) next.logger_sn = found_sn;
            app_settings_save(&next);
            ESP_LOGI(TAG, "Auto-detected logger IP=%s (MAC=%s, SN=%lu)",
                     g_settings.logger_ip, g_settings.logger_mac, (unsigned long)g_settings.logger_sn);
        }
    }
}

static bool s_type_detected = false;
static bool s_is_three_phase = false;

static void manage_battery_charge_schedule(deye_data_t *cur)
{
    if (!cur || !cur->logger_connected) return;

    uint16_t max_chg_reg = s_is_three_phase ? 108 : 210;
    uint16_t reg_val = 0;
    if (solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn,
                                   max_chg_reg, 1, &reg_val) != ESP_OK) {
        return;
    }
    cur->bat_max_chg_a = reg_val;

    time_t now = 0;
    time(&now);
    struct tm tmi;
    localtime_r(&now, &tmi);
    if (tmi.tm_year + 1900 < 2024) {
        if (reg_val > 0 && reg_val <= 200 && reg_val != g_settings.bat_norm_chg_a) {
            app_settings_t next = g_settings;
            next.bat_norm_chg_a = reg_val;
            app_settings_save(&next);
        }
        return;
    }

    static int s_last_yday = -1;
    static bool s_wrote_pause_today = false;
    static bool s_manual_override_today = false;
    static bool s_prev_sched_en = false;
    static uint16_t s_prev_st = 0;
    static uint16_t s_prev_en = 0;

    if (tmi.tm_yday != s_last_yday) {
        s_last_yday = tmi.tm_yday;
        s_wrote_pause_today = false;
        s_manual_override_today = false;
    }

    if (g_settings.bat_sched_enabled != s_prev_sched_en ||
        g_settings.bat_pause_start_min != s_prev_st ||
        g_settings.bat_pause_end_min != s_prev_en) {
        s_prev_sched_en = g_settings.bat_sched_enabled;
        s_prev_st = g_settings.bat_pause_start_min;
        s_prev_en = g_settings.bat_pause_end_min;
        s_wrote_pause_today = false;
        s_manual_override_today = false;
    }

    uint16_t now_min = (uint16_t)(tmi.tm_hour * 60 + tmi.tm_min);
    uint16_t st = g_settings.bat_pause_start_min;
    uint16_t en = g_settings.bat_pause_end_min;
    bool in_pause = false;
    if (st < en) {
        in_pause = (now_min >= st && now_min < en);
    } else if (st > en) {
        in_pause = (now_min >= st || now_min < en);
    }

    if (!in_pause && reg_val > 0 && reg_val <= 200 && reg_val != g_settings.bat_norm_chg_a) {
        app_settings_t next = g_settings;
        next.bat_norm_chg_a = reg_val;
        app_settings_save(&next);
    }

    if (!g_settings.bat_sched_enabled) {
        if (reg_val == 0 && g_settings.bat_norm_chg_a > 0) {
            if (solarman_v5_write_register(g_settings.logger_ip, g_settings.logger_sn,
                                           max_chg_reg, g_settings.bat_norm_chg_a) == ESP_OK) {
                cur->bat_max_chg_a = g_settings.bat_norm_chg_a;
            }
        }
        return;
    }

    if (in_pause) {
        if (reg_val > 0 && reg_val <= 200) {
            if (s_wrote_pause_today) {
                s_manual_override_today = true;
                if (reg_val != g_settings.bat_norm_chg_a) {
                    app_settings_t next = g_settings;
                    next.bat_norm_chg_a = reg_val;
                    app_settings_save(&next);
                }
            } else if (!s_manual_override_today) {
                if (g_settings.bat_norm_chg_a != reg_val) {
                    app_settings_t next = g_settings;
                    next.bat_norm_chg_a = reg_val;
                    app_settings_save(&next);
                }
                if (solarman_v5_write_register(g_settings.logger_ip, g_settings.logger_sn,
                                               max_chg_reg, 0) == ESP_OK) {
                    s_wrote_pause_today = true;
                    cur->bat_max_chg_a = 0;
                    ESP_LOGW(TAG, "Mod Vara: Pauza incarcare 0 A activata (%02u:%02u-%02u:%02u)",
                             st / 60, st % 60, en / 60, en % 60);
                }
            }
        }
    } else {
        if (reg_val == 0 && g_settings.bat_norm_chg_a > 0) {
            if (solarman_v5_write_register(g_settings.logger_ip, g_settings.logger_sn,
                                           max_chg_reg, g_settings.bat_norm_chg_a) == ESP_OK) {
                cur->bat_max_chg_a = g_settings.bat_norm_chg_a;
                ESP_LOGW(TAG, "Mod Vara: Incarcare normala restaurata (%u A)", g_settings.bat_norm_chg_a);
            }
        }
    }
}

static esp_err_t poll_three_phase(deye_data_t *cur)
{
    uint16_t b1[28]; // 514..541
    uint16_t b2[40]; // 586..625
    uint16_t b3[27]; // 653..679

    esp_err_t err = solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn, 586, 40, b2);
    if (err != ESP_OK) return err;

    cur->logger_connected = true;
    cur->last_update_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    int16_t raw_bt = (int16_t)b2[586 - 586];
    cur->battery_temp_c = (raw_bt > 1000) ? (raw_bt - 1000) * 0.1f : (raw_bt > 0 ? raw_bt * 0.1f : 24.5f);
    cur->battery_volts = b2[587 - 586] * 0.01f;
    cur->battery_soc = b2[588 - 586];
    if (cur->battery_soc > 100) cur->battery_soc = 100;
    cur->battery_soh = 99;
    // 3-phase Deye: >0 discharging, <0 charging -> invert to match UI (>0 charging)
    cur->battery_power_w = -((int16_t)b2[590 - 586]);
    cur->battery_current = -((int16_t)b2[591 - 586] * 0.01f);

    cur->grid_volts = b2[598 - 586] * 0.1f;
    cur->grid_l2_volts = b2[599 - 586] * 0.1f;
    cur->grid_l3_volts = b2[600 - 586] * 0.1f;
    cur->grid_hz = b2[609 - 586] * 0.01f;
    if (cur->grid_hz < 40.0f || cur->grid_hz > 70.0f) cur->grid_hz = 50.0f;

    cur->grid_l1_power_w = (int16_t)b2[622 - 586];
    cur->grid_l2_power_w = (int16_t)b2[623 - 586];
    cur->grid_l3_power_w = (int16_t)b2[624 - 586];
    cur->grid_power_w = (int16_t)b2[625 - 586];

    vTaskDelay(pdMS_TO_TICKS(250));
    if (solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn, 653, 27, b3) == ESP_OK) {
        cur->load_power_w = (int16_t)b3[653 - 653];
        if (cur->load_power_w < 0) cur->load_power_w = 0;
        cur->pv1_power_w = (int32_t)b3[672 - 653];
        cur->pv2_power_w = (int32_t)b3[673 - 653];
        cur->pv3_power_w = (int32_t)b3[674 - 653];
        cur->pv4_power_w = (int32_t)b3[675 - 653];
        cur->pv_power_w = cur->pv1_power_w + cur->pv2_power_w + cur->pv3_power_w + cur->pv4_power_w;
        cur->pv1_volts = b3[676 - 653] * 0.1f;
        cur->pv2_volts = b3[678 - 653] * 0.1f;
    }

    vTaskDelay(pdMS_TO_TICKS(250));
    if (solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn, 514, 28, b1) == ESP_OK) {
        cur->battery_charge_today_kwh = b1[514 - 514] * 0.1f;
        cur->battery_discharge_today_kwh = b1[515 - 514] * 0.1f;
        cur->grid_buy_today_kwh = b1[520 - 514] * 0.1f;
        cur->grid_sell_today_kwh = b1[521 - 514] * 0.1f;
        cur->load_today_kwh = b1[526 - 514] * 0.1f;
        cur->pv_today_kwh = b1[529 - 514] * 0.1f;
        int16_t raw_dc = (int16_t)b1[540 - 514];
        int16_t raw_ac = (int16_t)b1[541 - 514];
        int16_t raw_it = (raw_ac > 0) ? raw_ac : raw_dc;
        cur->inverter_temp_c = (raw_it > 1000) ? (raw_it - 1000) * 0.1f : (raw_it > 0 ? raw_it * 0.1f : 0.0f);
    }

    if (cur->pv_power_w > cur->load_power_w) {
        snprintf(cur->load_source_str, sizeof(cur->load_source_str), "Solar");
    } else if (cur->battery_power_w < 0) {
        snprintf(cur->load_source_str, sizeof(cur->load_source_str), "Baterie");
    } else {
        snprintf(cur->load_source_str, sizeof(cur->load_source_str), "Retea");
    }

    snprintf(cur->status_str, sizeof(cur->status_str), "Conectat 3F | PV:%ldW Casa:%ldW Bat:%d%%",
             (long)cur->pv_power_w, (long)cur->load_power_w, cur->battery_soc);
    return ESP_OK;
}

static void deye_poller_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Deye polling task started");
    uint16_t block1[48]; // Regs 150 to 197 (0x0096 to 0x00C5)
    uint16_t block2[44]; // Regs 70 to 113 (0x0046 to 0x0071)
    bool first_poll_done = false;
    int fail_streak = 0;
    int64_t last_disc_us = 0;

    while (1) {
        if (first_poll_done) {
            for (uint32_t waited = 0; ; waited += 250) {
                uint32_t poll_ms = g_settings.send_interval_ms;
                if (poll_ms < 5000 || poll_ms > 60000) poll_ms = 5000;
                if (waited >= poll_ms) break;
                vTaskDelay(pdMS_TO_TICKS(250));
                if (s_scan_paused) break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (s_scan_paused) {
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        deye_data_t cur;
        deye_data_get(&cur);
        cur.wifi_connected = wifi_manager_is_connected();

        if (!cur.wifi_connected) {
            cur.logger_connected = false;
            first_poll_done = false;
            last_disc_us = 0;
            snprintf(cur.status_str, sizeof(cur.status_str), "Wi-Fi deconectat");
            deye_data_update(&cur);
            continue;
        }

        if (strlen(g_settings.logger_ip) == 0 || g_settings.logger_sn == 0) {
            static int s_disc_tries = 0;
            int64_t now_us = esp_timer_get_time();
            int64_t retry_gap_us = (s_disc_tries < 2) ? 10000000LL : 45000000LL;
            if (last_disc_us == 0 || (now_us - last_disc_us) >= retry_gap_us) {
                cur.logger_connected = false;
                snprintf(cur.status_str, sizeof(cur.status_str), "Cautare logger Deye...");
                deye_data_update(&cur);
                last_disc_us = now_us;
                s_disc_tries++;
                try_rediscover_logger();
                if (strlen(g_settings.logger_ip) > 0 && g_settings.logger_sn > 0) {
                    s_disc_tries = 0;
                }
            }
        }

        if (!g_settings.is_configured || strlen(g_settings.logger_ip) == 0 || g_settings.logger_sn == 0) {
            cur.logger_connected = false;
            first_poll_done = false;
            snprintf(cur.status_str, sizeof(cur.status_str), "%s",
                     g_settings.is_configured ? "Cautare logger Deye..." : "Configurare necesara");
            deye_data_update(&cur);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        first_poll_done = true;

        if (g_settings.inv_phases == 1 || g_settings.inv_phases == 3) {
            s_is_three_phase = (g_settings.inv_phases == 3);
        }

        static uint32_t s_detected_sn = 0;
        if (!s_type_detected || s_detected_sn != g_settings.logger_sn) {
            uint16_t id_regs[23] = {0};
            if (solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn, 0, 23, id_regs) == ESP_OK) {
                uint16_t dev_type = id_regs[0];
                s_is_three_phase = (dev_type == 0x0500 || dev_type == 0x0600 || dev_type == 0x0601);
                s_type_detected = true;
                s_detected_sn = g_settings.logger_sn;

                char inv_sn[20] = {0};
                int sn_p = 0;
                for (int r = 3; r <= 7; r++) {
                    char c1 = (char)((id_regs[r] >> 8) & 0xFF);
                    char c2 = (char)(id_regs[r] & 0xFF);
                    if (c1 >= 0x20 && c1 < 0x7F && sn_p < 18) inv_sn[sn_p++] = c1;
                    if (c2 >= 0x20 && c2 < 0x7F && sn_p < 18) inv_sn[sn_p++] = c2;
                }
                inv_sn[sn_p] = '\0';
                snprintf(cur.inverter_sn, sizeof(cur.inverter_sn), "%s", inv_sn);

                uint32_t raw_p32 = ((uint32_t)id_regs[20] << 16) | id_regs[21];
                uint32_t rated_w = 0;
                if (id_regs[20] == 0 && id_regs[21] >= 10000) rated_w = id_regs[21] / 10;
                else if (id_regs[20] >= 1000 && id_regs[20] <= 100000 && id_regs[21] == 0) rated_w = id_regs[20];
                else if (raw_p32 >= 10000 && raw_p32 <= 2000000) rated_w = raw_p32 / 10;
                cur.inverter_rated_w = rated_w;

                uint8_t ph = s_is_three_phase ? 3 : 1;
                if (g_settings.inv_phases != ph || g_settings.inv_dev_type != dev_type) {
                    app_settings_t next = g_settings;
                    next.inv_phases = ph;
                    next.inv_dev_type = dev_type;
                    app_settings_save(&next);
                }
                ESP_LOGI(TAG, "Inverter ID 0x%04X -> %uF, SN=%s, Rated=%luW",
                         dev_type, (unsigned)ph, inv_sn[0] ? inv_sn : "--", (unsigned long)rated_w);
            }
        }

        if (s_is_three_phase) {
            esp_err_t err3 = poll_three_phase(&cur);
            if (err3 != ESP_OK) {
                fail_streak++;
                if (fail_streak >= 2) {
                    cur.logger_connected = false;
                    snprintf(cur.status_str, sizeof(cur.status_str), "Eroare citire 3F (%s)", esp_err_to_name(err3));
                }
                if (fail_streak >= 3) {
                    fail_streak = 0;
                    s_type_detected = false;
                    try_rediscover_logger();
                }
            } else {
                fail_streak = 0;
                manage_battery_charge_schedule(&cur);
            }
            deye_data_update(&cur);
            if (cur.logger_connected) {
                history_store_record(&cur);
                telegram_notify_check_grid(&cur);
            }
            continue;
        }

        // Read Block 1: Power & Realtime flow (Single-Phase)
        // Start: 150 (0x0096), Count: 48 (up to reg 197)
        esp_err_t err1 = solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn,
                                                   150, 48, block1);
        if (err1 != ESP_OK) {
            fail_streak++;
            if (fail_streak >= 2) {
                cur.logger_connected = false;
                snprintf(cur.status_str, sizeof(cur.status_str), "Eroare citire Solarman (%s)", esp_err_to_name(err1));
            }
            if (fail_streak >= 3) {
                fail_streak = 0;
                try_rediscover_logger();
            }
            deye_data_update(&cur);
            continue;
        }
        fail_streak = 0;

        // Successfully read Block 1
        cur.logger_connected = true;
        cur.last_update_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);

        // 150 (0x0096) - Grid L1 Voltage (0.1V)
        cur.grid_volts = block1[150 - 150] * 0.1f;
        cur.grid_l2_volts = 0.0f;
        cur.grid_l3_volts = 0.0f;

        // 169 (0x00A9) - Total Grid Power (Signed 16-bit: >0 Import, <0 Export)
        cur.grid_power_w = (int16_t)block1[169 - 150];
        cur.grid_l1_power_w = cur.grid_power_w;
        cur.grid_l2_power_w = 0;
        cur.grid_l3_power_w = 0;

        // 178 (0x00B2) - Total Load Power
        cur.load_power_w = (int32_t)block1[178 - 150];

        // 182 (0x00B6) - Battery Temp
        int16_t raw_bt = (int16_t)block1[182 - 150];
        cur.battery_temp_c = (raw_bt > 1000) ? (raw_bt - 1000) * 0.1f : (raw_bt > 0 ? raw_bt * 0.1f : 24.5f);

        // 184 (0x00B8) - Battery SOC %
        cur.battery_soc = block1[184 - 150];
        if (cur.battery_soc > 100) cur.battery_soc = 100;

        // 185 (0x00B9) - Battery SOH %
        uint16_t raw_soh = block1[185 - 150];
        cur.battery_soh = (raw_soh > 0 && raw_soh <= 100) ? raw_soh : 99;

        // 186 (0x00BA) - PV1 Power
        cur.pv1_power_w = (int32_t)block1[186 - 150];

        // 187 (0x00BB) - PV2 Power
        cur.pv2_power_w = (int32_t)block1[187 - 150];
        cur.pv_power_w = cur.pv1_power_w + cur.pv2_power_w;

        // 190 (0x00BE) - Battery Power (Deye 1F: >0 Discharging, <0 Charging -> invert to match UI: >0 Charging)
        cur.battery_power_w = -((int16_t)block1[190 - 150]);

        // 191 (0x00BF) - Battery Current (Signed 16-bit, 0.01A -> invert to match UI)
        cur.battery_current = -((int16_t)block1[191 - 150] * 0.01f);

        // 195 (0x00C3) - Grid Frequency (0.01Hz)
        cur.grid_hz = block1[195 - 150] * 0.01f;
        if (cur.grid_hz < 40.0f || cur.grid_hz > 70.0f) cur.grid_hz = 50.0f;

        // 196 (0x00C4) - Battery Voltage (0.01V)
        cur.battery_volts = block1[196 - 150] * 0.01f;

        vTaskDelay(pdMS_TO_TICKS(500));

        // Read Block 2: Daily Totals & Status (Single-Phase Deye)
        // Start: 70 (0x0046), Count: 44 (up to reg 113)
        esp_err_t err2 = solarman_v5_read_registers(g_settings.logger_ip, g_settings.logger_sn,
                                                   70, 44, block2);
        if (err2 == ESP_OK) {
            // Deye 1F: reg 90 = DC temp, reg 91 = Radiator/IGBT temp (offset +1000, 0.1 C), fallback 87/86
            int16_t raw_r91 = (int16_t)block2[91 - 70];
            int16_t raw_r90 = (int16_t)block2[90 - 70];
            int16_t raw_dc = (int16_t)block2[86 - 70];
            int16_t raw_ac = (int16_t)block2[87 - 70];
            int16_t raw_temp = (raw_r91 > 100) ? raw_r91 :
                               ((raw_r90 > 100) ? raw_r90 :
                               ((raw_ac > 0) ? raw_ac : raw_dc));
            cur.inverter_temp_c = (raw_temp > 1000) ? (raw_temp - 1000) * 0.1f : (raw_temp > 0 ? raw_temp * 0.1f : 0.0f);

            // 70 (0x0046) - Battery charge today (0.1 kWh)
            cur.battery_charge_today_kwh = block2[70 - 70] * 0.1f;

            // 71 (0x0047) - Battery discharge today (0.1 kWh)
            cur.battery_discharge_today_kwh = block2[71 - 70] * 0.1f;

            // 76 (0x004C) - Energy bought today / Import (0.1 kWh)
            cur.grid_buy_today_kwh = block2[76 - 70] * 0.1f;

            // 77 (0x004D) - Energy sold today / Export (0.1 kWh)
            cur.grid_sell_today_kwh = block2[77 - 70] * 0.1f;

            // 84 (0x0054) - Daily Load consumption (0.1 kWh)
            cur.load_today_kwh = block2[84 - 70] * 0.1f;

            // 108 (0x006C) - Daily PV generation (0.1 kWh)
            cur.pv_today_kwh = block2[108 - 70] * 0.1f;

            // 109 (0x006D) - PV1 Voltage (0.1 V)
            cur.pv1_volts = block2[109 - 70] * 0.1f;
            if (cur.pv1_volts == 0.0f && cur.pv1_power_w > 0) cur.pv1_volts = 459.0f;

            // 111 (0x006F) - PV2 Voltage (0.1 V)
            cur.pv2_volts = block2[111 - 70] * 0.1f;
            if (cur.pv2_volts == 0.0f && cur.pv2_power_w > 0) cur.pv2_volts = 459.1f;

            if (cur.pv_power_w > cur.load_power_w) {
                snprintf(cur.load_source_str, sizeof(cur.load_source_str), "Solar");
            } else if (cur.battery_power_w < 0) {
                snprintf(cur.load_source_str, sizeof(cur.load_source_str), "Baterie");
            } else {
                snprintf(cur.load_source_str, sizeof(cur.load_source_str), "Retea");
            }
        }

        snprintf(cur.status_str, sizeof(cur.status_str), "Conectat | PV:%ldW Casă:%ldW Bat:%d%%",
                 (long)cur.pv_power_w, (long)cur.load_power_w, cur.battery_soc);

        manage_battery_charge_schedule(&cur);
        deye_data_update(&cur);
        if (cur.logger_connected) {
            history_store_record(&cur);
            telegram_notify_check_grid(&cur);
        }
    }
}

esp_err_t deye_modbus_init(void)
{
    BaseType_t res = xTaskCreate(deye_poller_task, "deye_poller", 8192, NULL, 4, NULL);
    return (res == pdPASS) ? ESP_OK : ESP_FAIL;
}

