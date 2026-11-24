#include "local_web_server.h"
#include "ac_control.h"
#include "app_config.h"
#include "cJSON.h"
#include "app_settings.h"
#include "deye_data.h"
#include "deye_modbus.h"
#include "history_store.h"
#include "logger_discovery.h"
#include "solarman_v5.h"
#include "sys_metrics.h"
#include "telegram_notify.h"
#include "wifi_manager.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "local_web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t icon_192_png_start[] asm("_binary_icon_192_png_start");
extern const uint8_t icon_192_png_end[] asm("_binary_icon_192_png_end");
extern const uint8_t icon_512_png_start[] asm("_binary_icon_512_png_start");
extern const uint8_t icon_512_png_end[] asm("_binary_icon_512_png_end");

static const char *PORTAL_HTML =
"<!DOCTYPE html><html lang='ro'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Configurare ESP32-S3 Deye Local</title>"
"<style>"
"*{box-sizing:border-box;font-family:-apple-system,Segoe UI,Roboto,sans-serif;}"
"body{margin:0;padding:16px;background:#0b1120;color:#f8fafc;}"
".card{max-width:460px;margin:0 auto;background:#111827;border:1px solid #1f2937;border-radius:16px;padding:18px;}"
"h2{margin:0;font-size:17px;color:#38bdf8;}"
".hdr{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:6px;}"
".btn-close{width:auto;margin:0;padding:6px 12px;background:#ef4444;color:#fff;border-radius:8px;font-size:12px;font-weight:700;flex-shrink:0;}"
"p{margin:0 0 14px 0;font-size:12px;color:#94a3b8;}"
"label{display:block;font-size:12px;font-weight:700;color:#cbd5e1;margin:10px 0 4px 0;}"
"input{width:100%;padding:10px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#fff;font-size:14px;}"
"button{width:100%;padding:11px;border:none;border-radius:10px;font-weight:700;font-size:14px;cursor:pointer;margin-top:12px;}"
".btn-scan{background:#1e293b;color:#38bdf8;border:1px solid #38bdf8;}"
".btn-save{background:#10b981;color:#052e16;margin-top:18px;}"
".btn-save:disabled{opacity:0.6;cursor:wait;}"
".nets{margin-top:8px;display:flex;flex-direction:column;gap:6px;max-height:180px;overflow-y:auto;}"
".net{padding:8px 10px;background:#0f172a;border:1px solid #334155;border-radius:8px;font-size:13px;display:flex;justify-content:space-between;cursor:pointer;}"
".net:hover{border-color:#38bdf8;}"
"#msg{margin-top:12px;font-size:13px;font-weight:700;text-align:center;line-height:1.4;}"
"</style></head><body><div class='card'>"
"<div class='hdr'><h2>&#9881; Configurare Wi-Fi ESP32-S3</h2>"
"<button type='button' class='btn-close' onclick='closePortal()'>Close</button></div>"
"<p>Alege reteaua Wi-Fi locala. Dupa conectare, ESP32-S3 cauta si se conecteaza automat la loggerul Deye (iar daca ai mai multe loggere, il poti selecta din Setari).</p>"
"<button class='btn-scan' onclick='scanWifi()'>Scaneaza Retele Wi-Fi</button>"
"<div class='nets' id='nets'></div>"
"<label>Retea Wi-Fi (SSID)</label><input id='ssid' placeholder='Selecteaza sau scrie SSID'>"
"<label>Parola Wi-Fi</label><input id='pass' type='password' placeholder='Parola Wi-Fi'>"
"<label>Cod PIN Setari (optional)</label><input id='pin' placeholder='Ex: 1234 (lasa gol pt. fara PIN)'>"
"<button class='btn-save' id='saveBtn' onclick='saveCfg()'>Conecteaza &amp; Salveaza Wi-Fi</button>"
"<div id='msg'></div></div>"
"<script>"
"function scanWifi(){"
"const box=document.getElementById('nets');box.innerHTML='Se scaneaza...';"
"fetch('/scan').then(r=>r.json()).then(arr=>{"
"box.innerHTML='';"
"if(!arr.length){box.innerHTML='<div class=net>Nicio retea gasita</div>';return;}"
"arr.forEach(n=>{"
"const d=document.createElement('div');d.className='net';"
"d.innerHTML='<span>'+n.ssid+'</span><span>'+n.rssi+' dBm</span>';"
"d.onclick=()=>{document.getElementById('ssid').value=n.ssid;document.getElementById('pass').focus();};"
"box.appendChild(d);"
"});}).catch(()=>{box.innerHTML='Eroare scanare';});}"
"function saveCfg(){"
"const btn=document.getElementById('saveBtn');"
"const m=document.getElementById('msg');"
"const ssid=document.getElementById('ssid').value.trim();"
"if(!ssid){m.style.color='#ef4444';m.innerText='Introdu numele retelei Wi-Fi!';return;}"
"btn.disabled=true;m.style.color='#38bdf8';"
"m.innerText='Se verifica conexiunea Wi-Fi... (max 12s)';"
"const p={wifi_ssid:ssid,wifi_password:document.getElementById('pass').value,admin_pin:document.getElementById('pin').value.trim()};"
"fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)})"
".then(r=>r.json()).then(d=>{"
"btn.disabled=false;"
"if(d.ok){"
"document.querySelector('.card').innerHTML="
"'<div style=\"text-align:center;padding:8px 0;\">'+"
"'<div style=\"font-size:40px;margin-bottom:8px;\">&#9989;</div>'+"
"'<h2 style=\"color:#10b981;margin-bottom:8px;\">Wi-Fi salvat!</h2>'+"
"'<p style=\"color:#cbd5e1;font-size:13px;margin-bottom:14px;\">Conexiune reusita! Hotspotul se inchide.</p>'+"
"'<div style=\"text-align:left;background:#0f172a;border:1px solid #1e293b;border-radius:12px;padding:12px;font-size:13px;line-height:1.6;color:#e2e8f0;\">'+"
"'Telefonul va reveni pe Wi-Fi-ul casei si va deschide automat aplicatia in ~10 secunde:<br><br>'+"
"'<a href=\"http://deye.local/\" style=\"display:inline-block;padding:10px 16px;background:#0284c7;color:#fff;border-radius:8px;text-decoration:none;font-weight:700;\">Deschide Deye Monitor Local</a>'+"
"'</div></div>';"
"setTimeout(()=>{window.location.href='http://deye.local/';},10000);}"
"else{m.style.color='#ef4444';m.innerText=d.error||'Eroare validare Wi-Fi!';}"
"}).catch(()=>{btn.disabled=false;m.style.color='#ef4444';m.innerText='Eroare comunicatie cu ESP32-S3';});}"
"function closePortal(){if(history.length>1 && document.referrer.indexOf(location.host)>=0){history.back();}else{window.location.href='/';}}"
"(()=>{let sx=0,sy=0,ok=false;document.addEventListener('touchstart',e=>{if(e.touches.length===1){sx=e.touches[0].clientX;sy=e.touches[0].clientY;ok=true;}},{passive:true});"
"document.addEventListener('touchend',e=>{if(!ok||!e.changedTouches.length)return;ok=false;const dx=e.changedTouches[0].clientX-sx,dy=e.changedTouches[0].clientY-sy;if(Math.abs(dx)>56&&Math.abs(dx)>Math.abs(dy)*1.35)closePortal();},{passive:true});})();"
"scanWifi();"
"</script></body></html>";

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    mdns_free();
    wifi_manager_prepare_reboot();
    esp_restart();
}

typedef struct {
    uint32_t ip;
    uint8_t fails;
    int64_t locked_until_us;
} pin_lock_entry_t;

static pin_lock_entry_t s_pin_locks[8] = {0};

static uint32_t get_client_ip_u32(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr;
    socklen_t len = sizeof(addr);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == 0) {
        if (addr.sin6_family == AF_INET) {
            return ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
        }
        return addr.sin6_addr.un.u32_addr[3];
    }
    return 0;
}

static pin_lock_entry_t *find_or_alloc_lock(uint32_t ip) {
    int free_idx = 0;
    for (int i = 0; i < 8; i++) {
        if (s_pin_locks[i].ip == ip) return &s_pin_locks[i];
        if (s_pin_locks[i].ip == 0) free_idx = i;
    }
    s_pin_locks[free_idx].ip = ip;
    s_pin_locks[free_idx].fails = 0;
    s_pin_locks[free_idx].locked_until_us = 0;
    return &s_pin_locks[free_idx];
}

static int get_pin_lock_remaining_sec(uint32_t ip) {
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 8; i++) {
        if (s_pin_locks[i].ip == ip && s_pin_locks[i].locked_until_us > now) {
            return (int)((s_pin_locks[i].locked_until_us - now) / 1000000LL) + 1;
        }
    }
    return 0;
}

static bool check_admin_pin_hdr(httpd_req_t *req) {
    if (g_settings.admin_pin[0] == '\0') return true;
    char hdr[24] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Pin", hdr, sizeof(hdr)) == ESP_OK &&
        strcmp(hdr, g_settings.admin_pin) == 0) {
        return true;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"PIN invalid sau lipsa\"}", HTTPD_RESP_USE_STRLEN);
    return false;
}

static bool json_get_str(const char *json, const char *key, char *out, size_t out_sz) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (*p != '\"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '\"' && i + 1 < out_sz) {
        if (*p == '\\' && *(p + 1)) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_get_u32(const char *json, const char *key, uint32_t *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!isdigit((unsigned char)*p) && *p != '-') return false;
    *out = (uint32_t)strtoul(p, NULL, 10);
    return true;
}

static void json_escape(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 2 < dst_sz; i++) {
        char c = src[i];
        if (c == '\"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if ((unsigned char)c >= 0x20) {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static esp_err_t read_req_body(httpd_req_t *req, char *buf, size_t buf_sz) {
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_sz) {
        return ESP_ERR_INVALID_SIZE;
    }
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char host[64] = {0};
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if (wifi_manager_is_ap_active() && (!wifi_manager_is_connected() || strncmp(host, "192.168.4.1", 11) == 0)) {
        return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    }

    size_t len = (size_t)(index_html_end - index_html_start);
    if (len > 0 && index_html_start[len - 1] == '\0') {
        len--;
    }
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t setup_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_ap_record_t records[16];
    uint16_t count = 16;
    esp_err_t err = wifi_manager_scan(records, &count);

    char *resp = malloc(2048);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t pos = 0;
    pos += snprintf(resp + pos, 2048 - pos, "[");
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; i++) {
            char esc_ssid[96];
            json_escape((const char *)records[i].ssid, esc_ssid, sizeof(esc_ssid));
            if (esc_ssid[0] == '\0') continue;
            if (pos > 1) pos += snprintf(resp + pos, 2048 - pos, ",");
            pos += snprintf(resp + pos, 2048 - pos,
                            "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                            esc_ssid, records[i].rssi, records[i].authmode);
            if (pos > 1900) break;
        }
    }
    snprintf(resp + pos, 2048 - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req) {
    char body[768];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    app_settings_t cfg = g_settings;
    char prev_ssid[33] = {0};
    strncpy(prev_ssid, g_settings.wifi_ssid, sizeof(prev_ssid) - 1);

    json_get_str(body, "wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    json_get_str(body, "wifi_password", cfg.wifi_password, sizeof(cfg.wifi_password));
    if (!json_get_str(body, "admin_pin", cfg.admin_pin, sizeof(cfg.admin_pin))) {
        cfg.admin_pin[0] = '\0';
    }
    bool has_lmac = json_get_str(body, "logger_mac", cfg.logger_mac, sizeof(cfg.logger_mac));
    bool has_lip = json_get_str(body, "logger_ip", cfg.logger_ip, sizeof(cfg.logger_ip));
    bool has_lsn = json_get_u32(body, "logger_sn", &cfg.logger_sn);
    if (strcmp(prev_ssid, cfg.wifi_ssid) != 0 && !has_lip && !has_lsn && !has_lmac) {
        cfg.logger_mac[0] = '\0';
        cfg.logger_ip[0] = '\0';
        cfg.logger_sn = 0;
        cfg.inv_phases = 0;
        cfg.inv_dev_type = 0;
    }
    uint32_t intv = 0;
    if (json_get_u32(body, "send_interval_ms", &intv) && intv >= 5000 && intv <= 60000) {
        cfg.send_interval_ms = intv;
    }

    httpd_resp_set_type(req, "application/json");

    if (cfg.wifi_ssid[0] == '\0') {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"SSID lipsa\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (!wifi_manager_test_connect(cfg.wifi_ssid, cfg.wifi_password, 12000)) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut conecta la Wi-Fi (verifica parola)\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cfg.is_configured = true;
    app_settings_save(&cfg);

    httpd_resp_send(req, "{\"ok\":true,\"redirect\":\"http://deye.local/\"}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t telemetry_get_handler(httpd_req_t *req) {
    deye_data_t d;
    deye_data_get(&d);
    sys_metrics_t sysm;
    sys_metrics_get(&sysm);

    char *buf = malloc(2560);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char esc_src[32];
    char esc_status[96];
    json_escape(d.load_source_str, esc_src, sizeof(esc_src));
    json_escape(d.status_str, esc_status, sizeof(esc_status));

    int lock_sec = get_pin_lock_remaining_sec(get_client_ip_u32(req));
    bool pin_req = (g_settings.admin_pin[0] != '\0');

    snprintf(buf, 2560,
        "{\"online\":%s,\"pin_required\":%s,\"pin_locked_sec\":%d,\"data\":{"
        "\"pv_power_w\":%ld,\"pv1_power_w\":%ld,\"pv2_power_w\":%ld,\"pv3_power_w\":%ld,\"pv4_power_w\":%ld,"
        "\"pv1_volts\":%.1f,\"pv2_volts\":%.1f,\"pv_today_kwh\":%.2f,\"pv_total_kwh\":%.2f,"
        "\"battery_power_w\":%ld,\"battery_soc\":%u,\"battery_soh\":%u,"
        "\"battery_volts\":%.1f,\"battery_current\":%.1f,\"battery_temp_c\":%.1f,\"battery_capacity_kwh\":%.1f,"
        "\"battery_charge_today_kwh\":%.2f,\"battery_discharge_today_kwh\":%.2f,"
        "\"battery_charge_total_kwh\":%.2f,\"battery_discharge_total_kwh\":%.2f,"
        "\"bat_max_chg_a\":%u,\"bat_norm_chg_a\":%u,\"bat_sched_enabled\":%s,"
        "\"bat_pause_start\":%u,\"bat_pause_end\":%u,\"tg_bms_alert\":%s,\"tg_grid_alert\":%s,"
        "\"tg_bat_freeze_alert\":%s,\"bat_freeze_temp\":%d,"
        "\"grid_power_w\":%ld,\"grid_volts\":%.1f,\"grid_l2_volts\":%.1f,\"grid_l3_volts\":%.1f,"
        "\"grid_l1_power_w\":%ld,\"grid_l2_power_w\":%ld,\"grid_l3_power_w\":%ld,\"grid_hz\":%.2f,"
        "\"grid_buy_today_kwh\":%.2f,\"grid_sell_today_kwh\":%.2f,"
        "\"grid_buy_total_kwh\":%.2f,\"grid_sell_total_kwh\":%.2f,"
        "\"grid_outages_7d\":%u,\"grid_outages_14d\":%u,\"grid_outages_30d\":%u,"
        "\"load_power_w\":%ld,\"load_today_kwh\":%.2f,\"load_total_kwh\":%.2f,"
        "\"load_peak_today_kw\":%.2f,\"load_avg_hourly_kw\":%.2f,\"load_pf\":%.2f,"
        "\"load_source_str\":\"%s\",\"inverter_status\":%u,\"inverter_temp_c\":%.1f,"
        "\"inverter_sn\":\"%s\",\"inverter_rated_w\":%lu,\"logger_sn\":%lu,\"logger_mac\":\"%s\","
        "\"inv_phases\":%u,\"inv_dev_type\":%u,"
        "\"cpu_pct\":%d,\"cpu_mhz\":%d,\"ram_pct\":%d,\"ram_used_kb\":%lu,\"ram_total_kb\":%lu,"
        "\"psram_pct\":%d,\"storage_pct\":%d,\"storage_used_kb\":%lu,\"storage_total_kb\":%lu,"
        "\"spiffs_used_kb\":%lu,\"spiffs_total_kb\":%lu,\"esp_temp_c\":%.1f,\"esp_temp_ok\":%s,"
        "\"wifi_connected\":%s,\"wifi_rssi\":%d,\"wifi_eco\":%s,\"wifi_tx_dbm\":%d,\"logger_connected\":%s,\"status_str\":\"%s\",\"last_update_sec\":%lu"
        "}}",
        d.logger_connected ? "true" : "false",
        pin_req ? "true" : "false",
        lock_sec,
        (long)d.pv_power_w, (long)d.pv1_power_w, (long)d.pv2_power_w, (long)d.pv3_power_w, (long)d.pv4_power_w,
        d.pv1_volts, d.pv2_volts, d.pv_today_kwh, d.pv_total_kwh,
        (long)d.battery_power_w, (unsigned)d.battery_soc, (unsigned)d.battery_soh,
        d.battery_volts, d.battery_current, d.battery_temp_c, d.battery_capacity_kwh,
        d.battery_charge_today_kwh, d.battery_discharge_today_kwh,
        d.battery_charge_total_kwh, d.battery_discharge_total_kwh,
        (unsigned)d.bat_max_chg_a, (unsigned)g_settings.bat_norm_chg_a,
        g_settings.bat_sched_enabled ? "true" : "false",
        (unsigned)g_settings.bat_pause_start_min, (unsigned)g_settings.bat_pause_end_min,
        g_settings.tg_bms_alert_enabled ? "true" : "false",
        g_settings.tg_grid_alert_enabled ? "true" : "false",
        g_settings.tg_bat_freeze_alert_enabled ? "true" : "false",
        (int)g_settings.bat_freeze_temp_c,
        (long)d.grid_power_w, d.grid_volts, d.grid_l2_volts, d.grid_l3_volts,
        (long)d.grid_l1_power_w, (long)d.grid_l2_power_w, (long)d.grid_l3_power_w, d.grid_hz,
        d.grid_buy_today_kwh, d.grid_sell_today_kwh,
        d.grid_buy_total_kwh, d.grid_sell_total_kwh,
        (unsigned)d.grid_outages_7d, (unsigned)d.grid_outages_14d, (unsigned)d.grid_outages_30d,
        (long)d.load_power_w, d.load_today_kwh, d.load_total_kwh,
        d.load_peak_today_kw, d.load_avg_hourly_kw, d.load_pf,
        esc_src, (unsigned)d.inverter_status, d.inverter_temp_c,
        d.inverter_sn, (unsigned long)d.inverter_rated_w, (unsigned long)g_settings.logger_sn, g_settings.logger_mac,
        (unsigned)(g_settings.inv_phases ? g_settings.inv_phases : 1), (unsigned)g_settings.inv_dev_type,
        sysm.cpu_pct, sysm.cpu_mhz, sysm.ram_pct, (unsigned long)sysm.ram_used_kb, (unsigned long)sysm.ram_total_kb,
        sysm.psram_pct, sysm.storage_pct, (unsigned long)sysm.storage_used_kb, (unsigned long)sysm.storage_total_kb,
        (unsigned long)sysm.spiffs_used_kb, (unsigned long)sysm.spiffs_total_kb,
        sysm.esp_temp_c, sysm.esp_temp_ok ? "true" : "false",
        wifi_manager_is_connected() ? "true" : "false", wifi_manager_get_rssi(),
        g_settings.wifi_eco_mode ? "true" : "false", wifi_manager_get_tx_dbm(),
        d.logger_connected ? "true" : "false",
        esc_status, (unsigned long)d.last_update_sec);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t history_get_handler(httpd_req_t *req) {
    char query[128] = {0};
    char period[16] = "day";
    char date_str[16] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "period", period, sizeof(period));
        httpd_query_key_value(query, "date", date_str, sizeof(date_str));
    }

    size_t buf_sz = 32768;
    char *buf = malloc(buf_sz);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    history_store_query(period, date_str, buf, buf_sz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t pin_verify_post_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (g_settings.admin_pin[0] == '\0') {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint32_t ip = get_client_ip_u32(req);
    int rem = get_pin_lock_remaining_sec(ip);
    if (rem > 0) {
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"locked\":true,\"remaining_sec\":%d,\"error\":\"Acces blocat! Mai asteapta %d min.\"}",
                 rem, (rem / 60) + 1);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char body[128];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    char pin_in[24] = {0};
    json_get_str(body, "pin", pin_in, sizeof(pin_in));
    pin_lock_entry_t *entry = find_or_alloc_lock(ip);

    if (strcmp(pin_in, g_settings.admin_pin) == 0) {
        entry->fails = 0;
        entry->locked_until_us = 0;
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    entry->fails++;
    if (entry->fails >= 3) {
        entry->fails = 0;
        entry->locked_until_us = esp_timer_get_time() + 3600LL * 1000000LL;
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req,
            "{\"ok\":false,\"locked\":true,\"remaining_sec\":3600,\"error\":\"Ai gresit PIN-ul de 3 ori! Acces blocat 1 ora.\"}",
            HTTPD_RESP_USE_STRLEN);
    } else {
        int left = 3 - entry->fails;
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"locked\":false,\"attempts_left\":%d,\"error\":\"PIN gresit! Mai ai %d incercari.\"}",
                 left, left);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static esp_err_t history_clear_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    bool ok = history_store_clear();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"Eroare stergere\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char buf[896];
    char esc_mac[32], esc_ip[32], esc_tok[128], esc_chat[48], esc_gmod[64], esc_wcity[64], gkey_mask[32] = "";
    json_escape(g_settings.logger_mac, esc_mac, sizeof(esc_mac));
    json_escape(g_settings.logger_ip, esc_ip, sizeof(esc_ip));
    json_escape(g_settings.tg_bot_token, esc_tok, sizeof(esc_tok));
    json_escape(g_settings.tg_chat_id, esc_chat, sizeof(esc_chat));
    json_escape(g_settings.gemini_model[0] ? g_settings.gemini_model : "gemini-flash-lite-latest",
                esc_gmod, sizeof(esc_gmod));
    json_escape(g_settings.weather_city, esc_wcity, sizeof(esc_wcity));
    size_t gk_len = strlen(g_settings.gemini_api_key);
    bool has_gkey = (gk_len >= 8);
    if (has_gkey) {
        snprintf(gkey_mask, sizeof(gkey_mask), "%.4s\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2%s",
                 g_settings.gemini_api_key, g_settings.gemini_api_key + gk_len - 4);
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    const char *part_lbl = running ? running->label : "ota_0";

    snprintf(buf, sizeof(buf),
             "{\"logger_mac\":\"%s\",\"logger_ip\":\"%s\",\"logger_sn\":%lu,"
             "\"inv_phases\":%u,\"inv_dev_type\":%u,\"wifi_eco_mode\":%s,"
             "\"send_interval_ms\":%lu,\"tg_bot_token\":\"%s\",\"tg_chat_id\":\"%s\","
             "\"gemini_key_set\":%s,\"gemini_api_key_masked\":\"%s\",\"gemini_model\":\"%s\","
             "\"weather_city\":\"%s\",\"weather_lat\":%.4f,\"weather_lon\":%.4f,"
             "\"fw_version\":\"%s (%s)\"}",
             esc_mac, esc_ip, (unsigned long)g_settings.logger_sn,
             (unsigned)g_settings.inv_phases, (unsigned)g_settings.inv_dev_type,
             g_settings.wifi_eco_mode ? "true" : "false",
             (unsigned long)g_settings.send_interval_ms, esc_tok, esc_chat,
             has_gkey ? "true" : "false",
             gkey_mask,
             esc_gmod,
             esc_wcity, g_settings.weather_lat, g_settings.weather_lon,
             FW_VERSION, part_lbl);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool json_get_bool(const char *json, const char *key, bool *out);
static bool json_get_float(const char *json, const char *key, float *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '\0') return false;
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char body[960];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    app_settings_t cfg = g_settings;
    char prev_ip[16] = {0};
    strncpy(prev_ip, g_settings.logger_ip, sizeof(prev_ip) - 1);
    uint32_t prev_sn = g_settings.logger_sn;

    json_get_str(body, "logger_mac", cfg.logger_mac, sizeof(cfg.logger_mac));
    json_get_str(body, "logger_ip", cfg.logger_ip, sizeof(cfg.logger_ip));
    json_get_u32(body, "logger_sn", &cfg.logger_sn);
    if (cfg.logger_sn != prev_sn || strcmp(cfg.logger_ip, prev_ip) != 0) {
        cfg.inv_phases = 0;
        cfg.inv_dev_type = 0;
    }
    json_get_str(body, "tg_bot_token", cfg.tg_bot_token, sizeof(cfg.tg_bot_token));
    json_get_str(body, "tg_chat_id", cfg.tg_chat_id, sizeof(cfg.tg_chat_id));

    char gkey_in[128] = {0};
    if (json_get_str(body, "gemini_api_key", gkey_in, sizeof(gkey_in))) {
        if (gkey_in[0] != '\0' && strcmp(gkey_in, "********") != 0 && !strstr(gkey_in, "\xE2\x80\xA2")) {
            strncpy(cfg.gemini_api_key, gkey_in, sizeof(cfg.gemini_api_key) - 1);
            cfg.gemini_api_key[sizeof(cfg.gemini_api_key) - 1] = '\0';
        }
    }
    char gmod_in[48] = {0};
    if (json_get_str(body, "gemini_model", gmod_in, sizeof(gmod_in)) && gmod_in[0] != '\0') {
        strncpy(cfg.gemini_model, gmod_in, sizeof(cfg.gemini_model) - 1);
        cfg.gemini_model[sizeof(cfg.gemini_model) - 1] = '\0';
    }
    uint32_t intv = 0;
    if (json_get_u32(body, "send_interval_ms", &intv) && intv >= 5000 && intv <= 60000) {
        cfg.send_interval_ms = intv;
    }
    bool eco = cfg.wifi_eco_mode;
    if (json_get_bool(body, "wifi_eco_mode", &eco)) {
        cfg.wifi_eco_mode = eco;
    }
    json_get_str(body, "weather_city", cfg.weather_city, sizeof(cfg.weather_city));
    float fval = 0.0f;
    if (json_get_float(body, "weather_lat", &fval)) cfg.weather_lat = fval;
    if (json_get_float(body, "weather_lon", &fval)) cfg.weather_lon = fval;

    app_settings_save(&cfg);
    wifi_manager_apply_power_mode();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static bool json_get_bool(const char *json, const char *key, bool *out) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) { *out = true; return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

static esp_err_t battery_config_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    char body[256];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }
    app_settings_t cfg = g_settings;
    bool bval = false;
    if (json_get_bool(body, "bat_sched_enabled", &bval)) cfg.bat_sched_enabled = bval;
    if (json_get_bool(body, "tg_bms_alert", &bval)) cfg.tg_bms_alert_enabled = bval;
    if (json_get_bool(body, "tg_grid_alert", &bval)) cfg.tg_grid_alert_enabled = bval;
    if (json_get_bool(body, "tg_bat_freeze_alert", &bval)) cfg.tg_bat_freeze_alert_enabled = bval;
    const char *pfrz = strstr(body, "\"bat_freeze_temp\"");
    if (pfrz) {
        const char *c = strchr(pfrz, ':');
        if (c) {
            int tv = (int)strtol(c + 1, NULL, 10);
            if (tv >= -10 && tv <= 25) cfg.bat_freeze_temp_c = (int8_t)tv;
        }
    }
    uint32_t uval = 0;
    if (json_get_u32(body, "bat_pause_start", &uval) && uval < 1440) cfg.bat_pause_start_min = (uint16_t)uval;
    if (json_get_u32(body, "bat_pause_end", &uval) && uval < 1440) cfg.bat_pause_end_min = (uint16_t)uval;
    app_settings_save(&cfg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t telegram_test_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    esp_err_t err = telegram_notify_send_test();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Completeaza mai intai Bot Token si Chat ID\"}",
                        HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut trimite (verifica Token/Chat ID/internet)\"}",
                        HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

#define MAX_SCAN_LOGGERS 8
static volatile bool s_logger_scanning = false;
static volatile bool s_logger_scan_done = false;
static discovered_logger_t s_scan_results[MAX_SCAN_LOGGERS];
static int s_scan_count = 0;

static void logger_scan_bg_task(void *arg) {
    deye_modbus_set_scan_paused(true);

    discovered_logger_t tmp[MAX_SCAN_LOGGERS];
    int n = logger_discover_all(tmp, MAX_SCAN_LOGGERS);
    if (n > 0) {
        memcpy(s_scan_results, tmp, sizeof(discovered_logger_t) * (size_t)n);
    }
    s_scan_count = n;
    s_logger_scan_done = true;
    s_logger_scanning = false;

    deye_modbus_set_scan_paused(false);
    vTaskDelete(NULL);
}

static esp_err_t logger_scan_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    if (!s_logger_scanning) {
        s_logger_scan_done = false;
        s_scan_count = 0;
        s_logger_scanning = true;
        if (xTaskCreate(logger_scan_bg_task, "logger_scan", 5120, NULL, 4, NULL) != pdPASS) {
            s_logger_scanning = false;
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"scanning\":true,\"done\":false,\"loggers\":[]}");
    return ESP_OK;
}

static esp_err_t logger_scan_status_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    char *buf = malloc(1536);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    bool scanning = s_logger_scanning;
    bool done = s_logger_scan_done;
    int n = done ? s_scan_count : 0;
    size_t pos = snprintf(buf, 1536, "{\"scanning\":%s,\"done\":%s,\"loggers\":[",
                          scanning ? "true" : "false", done ? "true" : "false");
    for (int i = 0; i < n; i++) {
        char esc_model[48];
        json_escape(s_scan_results[i].model, esc_model, sizeof(esc_model));
        pos += snprintf(buf + pos, 1536 - pos,
                        "%s{\"ip\":\"%s\",\"mac\":\"%s\",\"sn\":%lu,\"model\":\"%s\"}",
                        (i > 0) ? "," : "",
                        s_scan_results[i].ip, s_scan_results[i].mac,
                        (unsigned long)s_scan_results[i].sn, esc_model);
        if (pos > 1408) break;
    }
    snprintf(buf + pos, 1536 - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t chart_get_handler(httpd_req_t *req) {
    char *buf = malloc(8192);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    history_store_chart_json(buf, 8192);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

static esp_err_t chart_fasttest_post_handler(httpd_req_t *req) {
    history_store_enable_fast_test(true);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    if (req->content_len <= 0 || req->content_len > 0x300000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Dimensiune fisier .bin invalida\"}");
        return ESP_FAIL;
    }

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Partitie OTA indisponibila\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut porni OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char *buf = malloc(2048);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = (remaining > 2048) ? 2048 : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare receptie fisier\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare scriere Flash\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Imagine .bin invalida\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition esuat: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Eroare activare partitie OTA\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA reusit pe partitia '%s'! Repornesc...", update_part->label);
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

typedef struct {
    char state[16];
    int progress;
    char message[128];
    char bin_url[256];
} github_ota_status_t;

static github_ota_status_t s_gh_ota = {
    .state = "idle",
    .progress = 0,
    .message = "",
    .bin_url = GITHUB_OTA_BIN_URL
};

static bool extract_json_str(const char *json, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz) {
        if (*p == '\\' && p[1]) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

bool github_ota_check_latest(char *out_ver, size_t ver_sz, char *out_url, size_t url_sz, char *out_notes, size_t notes_sz) {
    esp_http_client_config_t cfg = {
        .url = GITHUB_OTA_VERSION_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len > 2048) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = esp_http_client_read(client, buf + total, sizeof(buf) - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!extract_json_str(buf, "version", out_ver, ver_sz)) return false;
    if (!extract_json_str(buf, "url", out_url, url_sz) || strncmp(out_url, "https://", 8) != 0) {
        snprintf(out_url, url_sz, "%s", GITHUB_OTA_BIN_URL);
    }
    extract_json_str(buf, "notes", out_notes, notes_sz);
    return out_ver[0] != '\0';
}

static esp_err_t ota_check_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");

    char latest[32] = "";
    char url[256] = GITHUB_OTA_BIN_URL;
    char notes[160] = "";
    if (!github_ota_check_latest(latest, sizeof(latest), url, sizeof(url), notes, sizeof(notes))) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Nu s-a putut contacta GitHub (verifica conexiunea la internet)\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    snprintf(s_gh_ota.bin_url, sizeof(s_gh_ota.bin_url), "%s", url);
    bool avail = (strcmp(latest, FW_VERSION) != 0);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"current\":\"%s\",\"latest\":\"%s\",\"update_available\":%s,\"notes\":\"%s\"}",
             FW_VERSION, latest, avail ? "true" : "false", notes);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void ota_github_task(void *arg) {
    (void)arg;
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "running");
    s_gh_ota.progress = 0;
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Conectare la GitHub...");

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Partitie OTA indisponibila");
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_config_t cfg = {
        .url = s_gh_ota.bin_url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare initializare HTTP client");
        vTaskDelete(NULL);
        return;
    }

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Nu s-a putut deschide conexiunea HTTPS");
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_len <= 0 || content_len > 0x300000) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Fisier firmware indisponibil (HTTP %d)", status);
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare pornire scriere Flash");
        vTaskDelete(NULL);
        return;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Memorie insuficienta");
        vTaskDelete(NULL);
        return;
    }

    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Se descarca firmware de pe GitHub...");
    int written = 0;
    while (written < content_len) {
        int r = esp_http_client_read(client, buf, 4096);
        if (r < 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
            snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Conexiune intrerupta in timpul descarcarii");
            vTaskDelete(NULL);
            return;
        }
        if (r == 0) break;
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
            snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare scriere Flash");
            vTaskDelete(NULL);
            return;
        }
        written += r;
        s_gh_ota.progress = (written * 100) / content_len;
    }

    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (written != content_len || esp_ota_end(ota_handle) != ESP_OK) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Imagine .bin incompleta sau invalida");
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK) {
        snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "error");
        snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Eroare activare partitie OTA");
        vTaskDelete(NULL);
        return;
    }

    s_gh_ota.progress = 100;
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "done");
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Actualizare reusita! ESP32-S3 reporneste...");
    ESP_LOGI(TAG, "GitHub OTA reusit pe '%s'! Repornesc in 3.0s...", update_part->label);
    vTaskDelay(pdMS_TO_TICKS(3000));
    mdns_free();
    wifi_manager_prepare_reboot();
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t ota_github_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    if (strcmp(s_gh_ota.state, "running") == 0) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Actualizarea ruleaza deja\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    snprintf(s_gh_ota.state, sizeof(s_gh_ota.state), "running");
    s_gh_ota.progress = 0;
    snprintf(s_gh_ota.message, sizeof(s_gh_ota.message), "Pornire actualizare GitHub...");
    xTaskCreate(ota_github_task, "ota_github", 8192, NULL, 5, NULL);
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ota_status_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
             s_gh_ota.state, s_gh_ota.progress, s_gh_ota.message);
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req) {
    if (wifi_manager_is_ap_active()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

static void captive_dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[256];
    while (1) {
        struct sockaddr_in client = {0};
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&client, &clen);
        if (len < 12 || !wifi_manager_is_ap_active()) continue;

        int q = 12;
        while (q < len && buf[q] != 0) q += buf[q] + 1;
        q += 5;
        if (q > len || q + 16 > (int)sizeof(buf)) continue;

        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0x00; buf[7] = 0x01;
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;
        int idx = q;
        buf[idx++] = 0xC0; buf[idx++] = 0x0C;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x01;
        buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 0x00; buf[idx++] = 1;
        buf[idx++] = 0x00; buf[idx++] = 0x04;
        buf[idx++] = 192;  buf[idx++] = 168;  buf[idx++] = 4;    buf[idx++] = 1;
        sendto(sock, buf, idx, 0, (struct sockaddr *)&client, clen);
    }
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    xTaskCreate(restart_task, "web_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void json_escape_multiline(const char *src, char *dst, size_t dst_sz) {
    size_t j = 0;
    if (!src || dst_sz == 0) {
        if (dst_sz > 0) dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 3 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\"' || c == '\\') {
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            continue;
        } else if (c == '\t') {
            dst[j++] = ' ';
        } else if (c >= 0x20) {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

static const char *ro_weekday_from_ymd(const char *ymd) {
    static const char *DAYS[7] = {
        "Duminică", "Luni", "Marți", "Miercuri", "Joi", "Vineri", "Sâmbătă"
    };
    int y = 0, m = 0, d = 0;
    if (!ymd || sscanf(ymd, "%d-%d-%d", &y, &m, &d) != 3) return "";
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    int dow = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    if (dow < 0 || dow > 6) return "";
    return DAYS[dow];
}

static const char *wmo_weather_ro(int code) {
    if (code == 0) return "Senin";
    if (code <= 2) return "Parțial noros";
    if (code == 3) return "Noros";
    if (code == 45 || code == 48) return "Ceață";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Ploaie";
    if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return "Ninsoare";
    if (code >= 95) return "Furtună";
    return "Variabil";
}

static bool json_parse_num_arr(const char *sec, const char *key, float *arr, int max_n) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(sec, pat);
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    for (int i = 0; i < max_n; i++) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r')) p++;
        if (*p == ']' || *p == '\0') return (i > 0);
        char *end = NULL;
        arr[i] = strtof(p, &end);
        if (end == p) {
            // handle null
            if (strncmp(p, "null", 4) == 0) { arr[i] = 0.0f; p += 4; }
            else return (i > 0);
        } else {
            p = end;
        }
    }
    return true;
}

static bool json_parse_date_arr(const char *sec, const char *key, char dates[][12], int max_n) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(sec, pat);
    if (!p) return false;
    p = strchr(p, '[');
    if (!p) return false;
    p++;
    for (int i = 0; i < max_n; i++) {
        const char *q1 = strchr(p, '\"');
        const char *br = strchr(p, ']');
        if (!q1 || (br && q1 > br)) return (i > 0);
        q1++;
        const char *q2 = strchr(q1, '\"');
        if (!q2) return (i > 0);
        size_t n = (size_t)(q2 - q1);
        if (n >= 12) n = 11;
        memcpy(dates[i], q1, n);
        dates[i][n] = '\0';
        p = q2 + 1;
    }
    return true;
}

static void fetch_open_meteo_7d(char *out, size_t out_sz) {
    if (out_sz == 0) return;
    out[0] = '\0';
    if (g_settings.weather_lat == 0.0f && g_settings.weather_lon == 0.0f) {
        snprintf(out, out_sz,
                 "- PROGNOZĂ METEO (Open-Meteo): Localitatea nu este configurată în Setări (alege orașul în tabul Setări).");
        return;
    }

    static char *s_meteo_cache = NULL;
    static int64_t s_meteo_ts = 0;
    static float s_last_lat = 0.0f, s_last_lon = 0.0f;
    int64_t now_us = esp_timer_get_time();

    if (s_meteo_cache && (now_us - s_meteo_ts) < 1800000000LL &&
        s_last_lat == g_settings.weather_lat && s_last_lon == g_settings.weather_lon) {
        snprintf(out, out_sz, "%s", s_meteo_cache);
        return;
    }

    if (!wifi_manager_is_connected()) {
        snprintf(out, out_sz, "- PROGNOZĂ METEO (Open-Meteo): Wi-Fi deconectat.");
        return;
    }

    char url[340];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,cloud_cover,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,shortwave_radiation_sum"
             "&timezone=auto&past_days=15&forecast_days=7",
             g_settings.weather_lat, g_settings.weather_lon);

    char *resp = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!resp) resp = calloc(1, 8192);
    if (!resp) return;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    int total = 0, status = 0;
    if (client) {
        if (esp_http_client_open(client, 0) == ESP_OK) {
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status == 200) {
                while (total < 8188) {
                    int r = esp_http_client_read(client, resp + total, 8188 - total);
                    if (r <= 0) break;
                    total += r;
                }
                resp[total] = '\0';
            }
            esp_http_client_close(client);
        }
        esp_http_client_cleanup(client);
    }

    if (status != 200 || total == 0) {
        free(resp);
        snprintf(out, out_sz, "- PROGNOZĂ METEO (Open-Meteo): Indisponibilă temporar.");
        return;
    }

    const char *cur_sec = strstr(resp, "\"current\":{");
    const char *day_sec = strstr(resp, "\"daily\":{");
    float cur_t = 0.0f, cur_cld = 0.0f, cur_wc = 0.0f;
    if (cur_sec) {
        json_get_float(cur_sec, "temperature_2m", &cur_t);
        json_get_float(cur_sec, "cloud_cover", &cur_cld);
        json_get_float(cur_sec, "weather_code", &cur_wc);
    }

    char dates[22][12] = {{0}};
    float wc[22] = {0}, tmax[22] = {0}, tmin[22] = {0}, rain[22] = {0}, rad_mj[22] = {0};
    bool ok_d = day_sec &&
                json_parse_date_arr(day_sec, "time", dates, 22) &&
                json_parse_num_arr(day_sec, "weather_code", wc, 22) &&
                json_parse_num_arr(day_sec, "temperature_2m_max", tmax, 22) &&
                json_parse_num_arr(day_sec, "temperature_2m_min", tmin, 22) &&
                json_parse_num_arr(day_sec, "precipitation_sum", rain, 22) &&
                json_parse_num_arr(day_sec, "shortwave_radiation_sum", rad_mj, 22);

    if (!ok_d) {
        free(resp);
        snprintf(out, out_sz, "- PROGNOZĂ METEO (Open-Meteo): Eroare parsare date.");
        return;
    }

    int total_days = 0;
    while (total_days < 22 && dates[total_days][0] != '\0') total_days++;
    int start_fc = (total_days >= 7) ? (total_days - 7) : 0;

    // Learn empirical yield from up to the past 15 days of actual inverter history + Open-Meteo weather/radiation
    int calib_days = 0, n_clr = 0, n_prt = 0, n_cld = 0;
    float sum_hist_pv = 0.0f, sum_hist_rad = 0.0f;
    float sum_clr = 0.0f, sum_prt = 0.0f, sum_cld = 0.0f;
    for (int i = 0; i < start_fc; i++) {
        char hbuf[220] = {0};
        history_store_query("day", dates[i], hbuf, sizeof(hbuf));
        bool has_data = false;
        float pv_kwh = 0.0f;
        if (json_get_bool(hbuf, "has_data", &has_data) && has_data &&
            json_get_float(hbuf, "pv_kwh", &pv_kwh) && pv_kwh > 0.05f) {
            float rad_kwh = rad_mj[i] / 3.6f;
            if (rad_kwh > 0.10f) {
                sum_hist_pv += pv_kwh;
                sum_hist_rad += rad_kwh;
                calib_days++;
                int code = (int)wc[i];
                if (code == 0) { sum_clr += pv_kwh; n_clr++; }
                else if (code <= 2) { sum_prt += pv_kwh; n_prt++; }
                else { sum_cld += pv_kwh; n_cld++; }
            }
        }
    }

    if (!s_meteo_cache) {
        s_meteo_cache = heap_caps_calloc(1, 1600, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_meteo_cache) s_meteo_cache = calloc(1, 1600);
    }
    char *dst = s_meteo_cache ? s_meteo_cache : out;
    size_t dst_sz = s_meteo_cache ? 1600 : out_sz;

    const char *cname = g_settings.weather_city[0] ? g_settings.weather_city : "Localitate";
    bool use_calib = (calib_days >= 1 && sum_hist_rad > 0.2f);
    float kwh_per_rad = use_calib ? (sum_hist_pv / sum_hist_rad) : 0.0f;

    int off = snprintf(dst, dst_sz,
                       "- METEO ACUM (%s, %.2fN %.2fE): %s, %.1f C, nori %.0f%%\n",
                       cname, g_settings.weather_lat, g_settings.weather_lon,
                       wmo_weather_ro((int)cur_wc), cur_t, cur_cld);
    if (use_calib && off > 0 && (size_t)off < dst_sz - 160) {
        off += snprintf(dst + off, dst_sz - (size_t)off,
                        "- ÎNVĂȚARE DIN ISTORIC (%d zile din ultimele 15): %.1f kWh produși / %.2f kWh/m2 -> factor sistem %.2f kWh/(kWh/m2)",
                        calib_days, sum_hist_pv, sum_hist_rad, kwh_per_rad);
        if (n_clr > 0) off += snprintf(dst + off, dst_sz - (size_t)off, " | medie Senin: %.1f kWh/zi", sum_clr / n_clr);
        if (n_prt > 0) off += snprintf(dst + off, dst_sz - (size_t)off, " | Parțial noros: %.1f kWh/zi", sum_prt / n_prt);
        if (n_cld > 0) off += snprintf(dst + off, dst_sz - (size_t)off, " | Înnorat/Ploaie: %.1f kWh/zi", sum_cld / n_cld);
        off += snprintf(dst + off, dst_sz - (size_t)off, "\n");
    } else if (off > 0 && (size_t)off < dst_sz - 140) {
        off += snprintf(dst + off, dst_sz - (size_t)off,
                        "- ÎNVĂȚARE DIN ISTORIC: 0 zile cu producție reală în ultimele 15 zile (estimarea devine automată după acumularea istoricului).\n");
    }
    if (off > 0 && (size_t)off < dst_sz - 64) {
        off += snprintf(dst + off, dst_sz - (size_t)off, "- PROGNOZĂ 7 ZILE (Open-Meteo):\n");
    }

    float sum_est_kwh = 0.0f;
    for (int i = start_fc; i < total_days && off > 0 && (size_t)off < dst_sz - 120; i++) {
        float rad_kwh = rad_mj[i] / 3.6f;
        const char *dow = ro_weekday_from_ymd(dates[i]);
        if (use_calib) {
            float est_kwh = rad_kwh * kwh_per_rad;
            sum_est_kwh += est_kwh;
            off += snprintf(dst + off, dst_sz - (size_t)off,
                            "  %s %s: %s, %.0f..%.0f C, ploaie %.1f mm, rad %.2f kWh/m2 -> estimat ~%.1f kWh\n",
                            dow, dates[i], wmo_weather_ro((int)wc[i]), tmin[i], tmax[i], rain[i], rad_kwh, est_kwh);
        } else {
            off += snprintf(dst + off, dst_sz - (size_t)off,
                            "  %s %s: %s, %.0f..%.0f C, ploaie %.1f mm, rad %.2f kWh/m2\n",
                            dow, dates[i], wmo_weather_ro((int)wc[i]), tmin[i], tmax[i], rain[i], rad_kwh);
        }
    }
    if (use_calib && off > 0 && (size_t)off < dst_sz - 64) {
        snprintf(dst + off, dst_sz - (size_t)off,
                 "  Total estimat PV 7 zile (calibrat pe istoric): ~%.1f kWh\n", sum_est_kwh);
    }

    if (s_meteo_cache) {
        s_meteo_ts = now_us;
        s_last_lat = g_settings.weather_lat;
        s_last_lon = g_settings.weather_lon;
        snprintf(out, out_sz, "%s", s_meteo_cache);
    }
    free(resp);
}

static void build_inverter_ai_context(const char *extra_bat_info, char *out, size_t out_sz) {
    deye_data_t d;
    deye_data_get(&d);

    char *meteo = heap_caps_calloc(1, 1400, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!meteo) meteo = calloc(1, 1400);
    if (meteo) fetch_open_meteo_7d(meteo, 1400);

    time_t now = time(NULL);
    struct tm tmi = {0};
    localtime_r(&now, &tmi);
    char day_s[16] = "2026-11-22", mon_s[12] = "2026-11", yr_s[8] = "2026";
    if (tmi.tm_year + 1900 >= 2024) {
        snprintf(day_s, sizeof(day_s), "%04d-%02d-%02d", tmi.tm_year + 1900, tmi.tm_mon + 1, tmi.tm_mday);
        snprintf(mon_s, sizeof(mon_s), "%04d-%02d", tmi.tm_year + 1900, tmi.tm_mon + 1);
        snprintf(yr_s, sizeof(yr_s), "%04d", tmi.tm_year + 1900);
    }

    char h_day[256] = {0}, h_mon[256] = {0}, h_yr[256] = {0}, h_tot[256] = {0};
    history_store_query("day", day_s, h_day, sizeof(h_day));
    history_store_query("month", mon_s, h_mon, sizeof(h_mon));
    history_store_query("year", yr_s, h_yr, sizeof(h_yr));
    history_store_query("total", day_s, h_tot, sizeof(h_tot));

    if (!d.logger_connected) {
        snprintf(out, out_sz,
                 "DATE SISTEM DEYE (%s):\n"
                 "- ATENTIE: Invertorul / loggerul Deye este DECONECTAT (placa ESP32-S3 nu a primit date live de la invertor). Nu raporta valori live ca fiind reale.\n"
                 "- Parametri autonomie setati in interfata: %s\n"
                 "- Istoric Zi (%s): %s\n"
                 "- Istoric Luna (%s): %s\n"
                 "- Istoric An (%s): %s\n"
                 "- Istoric Total: %s\n%s",
                 day_s,
                 (extra_bat_info && extra_bat_info[0]) ? extra_bat_info : "nespecificat",
                 day_s, h_day, mon_s, h_mon, yr_s, h_yr, h_tot,
                 meteo ? meteo : "");
        free(meteo);
        return;
    }

    snprintf(out, out_sz,
             "DATE SISTEM DEYE (%s):\n"
             "- Conectat: %s, Tip: %s (%lu W), Temp invertor: %.1f C, SN: %s\n"
             "- Solar PV acum: %ld W (PV1: %ld W / %.0f V, PV2: %ld W / %.0f V), Azi: %.2f kWh, Total: %.1f kWh\n"
             "- Baterie: SoC %u%%, SOH %u%%, Putere %ld W (+incarcare/-descarcare), %.2f V, %.1f A, Temp %.1f C, "
             "Limita incarcare: %u A (normal: %u A, Pauza Mod Vara: %s %02u:%02u-%02u:%02u)\n"
             "- Retea: %ld W (+import/-export), %.1f V (L2: %.1f V, L3: %.1f V), %.2f Hz, "
             "Import azi: %.2f kWh, Export azi: %.2f kWh (EN 50549-1: 195.5V-253V normal, >253V supratensiune)\n"
             "- Consum Casa: %ld W (Sursa: %s), Azi: %.2f kWh, Varf azi: %.2f kW, Medie orara: %.2f kW\n"
             "- Parametri autonomie utilizator: %s\n"
             "- Istoric Zi (%s): %s\n"
             "- Istoric Luna (%s): %s\n"
             "- Istoric An (%s): %s\n"
             "- Istoric Total: %s\n%s",
             day_s,
             d.logger_connected ? "DA" : "NU",
             (g_settings.inv_phases == 3) ? "Trifazat 3F" : "Monofazat 1F",
             (unsigned long)d.inverter_rated_w, d.inverter_temp_c, d.inverter_sn,
             (long)d.pv_power_w, (long)d.pv1_power_w, d.pv1_volts, (long)d.pv2_power_w, d.pv2_volts,
             d.pv_today_kwh, d.pv_total_kwh,
             (unsigned)d.battery_soc, (unsigned)d.battery_soh, (long)d.battery_power_w,
             d.battery_volts, d.battery_current, d.battery_temp_c,
             (unsigned)d.bat_max_chg_a, (unsigned)g_settings.bat_norm_chg_a,
             g_settings.bat_sched_enabled ? "ACTIV" : "INACTIV",
             (unsigned)(g_settings.bat_pause_start_min / 60), (unsigned)(g_settings.bat_pause_start_min % 60),
             (unsigned)(g_settings.bat_pause_end_min / 60), (unsigned)(g_settings.bat_pause_end_min % 60),
             (long)d.grid_power_w, d.grid_volts, d.grid_l2_volts, d.grid_l3_volts, d.grid_hz,
             d.grid_buy_today_kwh, d.grid_sell_today_kwh,
             (long)d.load_power_w, d.load_source_str, d.load_today_kwh, d.load_peak_today_kw, d.load_avg_hourly_kw,
             (extra_bat_info && extra_bat_info[0]) ? extra_bat_info : "standard",
             day_s, h_day, mon_s, h_mon, yr_s, h_yr, h_tot,
             meteo ? meteo : "");
    free(meteo);
}

static bool gemini_extract_text(const char *json, char *out, size_t out_sz) {
    if (!json || !out || out_sz == 0) return false;
    out[0] = '\0';
    const char *cand = strstr(json, "\"candidates\"");
    if (!cand) return false;

    size_t j = 0;
    const char *search = cand;
    while (j + 1 < out_sz) {
        const char *p = strstr(search, "\"text\"");
        if (!p) break;
        // Skip internal model thinking parts ("thought": true)
        const char *brace = p;
        while (brace > cand && *brace != '{') brace--;
        const char *th = strstr(brace, "\"thought\"");
        if (th && th < p) {
            search = p + 6;
            continue;
        }

        p += 6;
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
        if (*p != '\"') {
            search = p;
            continue;
        }
        p++;

        if (j > 0 && j + 2 < out_sz) out[j++] = '\n';
        while (*p && *p != '\"' && j + 1 < out_sz) {
            if (*p == '\\' && *(p + 1)) {
                p++;
                if (*p == 'n') out[j++] = '\n';
                else if (*p == 't') out[j++] = ' ';
                else if (*p == 'r') { /* skip */ }
                else if (*p == '\"' || *p == '\\' || *p == '/') out[j++] = *p;
                else out[j++] = *p;
                p++;
            } else if (*p == '*' && *(p + 1) == '*') {
                p += 2;
            } else if (*p == '`') {
                p++;
            } else {
                out[j++] = *p++;
            }
        }
        search = (*p == '\"') ? p + 1 : p;
    }
    out[j] = '\0';
    return (j > 0);
}

static const char *GOOGLE_ROOT_CA_PEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n"
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n"
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n"
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n"
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n"
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n"
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7\n"
"wCl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zU\n"
"jwTcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pf\n"
"yblqAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtc\n"
"vfaHszVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo\n"
"4Zor8Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkU\n"
"spzBmkMiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12\n"
"LOORc92wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLe\n"
"XdYWk70paDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg\n"
"8QOB+DVrNVjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrn\n"
"qYNxgFlQIDAQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB\n"
"/zAdBgNVHQ4EFgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQAD\n"
"ggIBAJ+qQibbC5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJy\n"
"q+bXmYOfg6LEeQkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5D\n"
"Ukzps+GlQebtuyh6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYae\n"
"lpkttGsN/H9oPM47HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV\n"
"3s6MJT/KYnAK9y8JZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueI\n"
"HoI17eko8cdLiA6EfMgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5\n"
"meLMFrUKTX5hgUvYU/Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhF\n"
"RAbd03OIozUhfJFfbdT6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ\n"
"9r/tSQdirpLsQBqvFAnZ0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5l\n"
"ndcEZOitHTtNCjv0xyBZm2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/\n"
"PeEMMA3KgqlbbC1j+Qa3bbbP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
"VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
"A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
"WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
"IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
"AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
"QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKv\n"
"RHYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
"BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
"9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
"p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
"-----END CERTIFICATE-----\n";

static bool gemini_discover_flash_model(const char *api_key, char *out_model, size_t out_sz) {
    esp_http_client_config_t cfg = {
        .url = "https://generativelanguage.googleapis.com/v1beta/models?pageSize=40",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 12000,
        .buffer_size = 4096,
        .cert_pem = GOOGLE_ROOT_CA_PEM,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;
    esp_http_client_set_header(client, "x-goog-api-key", api_key);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *rbuf = calloc(1, 8192);
    int total = 0;
    if (rbuf && status == 200) {
        while (total < 8190) {
            int r = esp_http_client_read(client, rbuf + total, 8190 - total);
            if (r <= 0) break;
            total += r;
        }
        rbuf[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool found = false;
    if (rbuf && total > 0) {
        const char *p = rbuf;
        char fallback_cand[48] = {0};
        while ((p = strstr(p, "\"models/gemini-")) != NULL) {
            p += strlen("\"models/");
            const char *end = strchr(p, '\"');
            if (end && (size_t)(end - p) < out_sz) {
                char cand[48] = {0};
                memcpy(cand, p, (size_t)(end - p));
                if (strstr(cand, "flash") && !strstr(cand, "image") && !strstr(cand, "tts") && !strstr(cand, "audio") && !strstr(cand, "thinking") && !strstr(cand, "2.5") && !strstr(cand, "2.0") && !strstr(cand, "1.5") && !strstr(cand, "1.0")) {
                    if (strstr(cand, "gemini-3")) {
                        strncpy(out_model, cand, out_sz - 1);
                        out_model[out_sz - 1] = '\0';
                        found = true;
                        break;
                    }
                    if (!fallback_cand[0]) {
                        strncpy(fallback_cand, cand, sizeof(fallback_cand) - 1);
                    }
                }
            }
            p = end ? end + 1 : p + 1;
        }
        if (!found && fallback_cand[0]) {
            strncpy(out_model, fallback_cand, out_sz - 1);
            out_model[out_sz - 1] = '\0';
            found = true;
        }
    }
    free(rbuf);
    return found;
}

static void *ai_alloc_buf(size_t sz) {
    void *p = heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, sz);
}

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int tls_err;
    int mbed_err;
    int sock_err;
} gem_http_ctx_t;

static esp_err_t gemini_http_evt_cb(esp_http_client_event_t *evt) {
    gem_http_ctx_t *g = (gem_http_ctx_t *)evt->user_data;
    if (!g) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        size_t avail = (g->cap > g->len + 1) ? (g->cap - 1 - g->len) : 0;
        size_t copy_n = ((size_t)evt->data_len < avail) ? (size_t)evt->data_len : avail;
        if (copy_n > 0) {
            memcpy(g->buf + g->len, evt->data, copy_n);
            g->len += copy_n;
            g->buf[g->len] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ERROR && evt->data) {
        esp_tls_last_error_t *te = (esp_tls_last_error_t *)evt->data;
        g->tls_err = te->last_error;
        g->mbed_err = te->esp_tls_error_code;
        g->sock_err = te->esp_tls_flags;
    }
    return ESP_OK;
}

static int gemini_call_single(const char *api_key, const char *model,
                              const char *full_prompt, char *out_reply, size_t out_reply_sz) {
    if (!wifi_manager_is_connected()) {
        snprintf(out_reply, out_reply_sz, "Wi-Fi deconectat. Conecteaza ESP32-S3 la internet.");
        return -1;
    }

    char url[192];
    snprintf(url, sizeof(url),
             "https://generativelanguage.googleapis.com/v1beta/models/%s:generateContent",
             model);

    char *esc_prompt = ai_alloc_buf(6400);
    char *req_json = ai_alloc_buf(6900);
    char *resp_buf = ai_alloc_buf(16384);
    if (!esc_prompt || !req_json || !resp_buf) {
        free(esc_prompt); free(req_json); free(resp_buf);
        snprintf(out_reply, out_reply_sz, "Memorie RAM insuficienta pe ESP32-S3.");
        return -1;
    }

    json_escape_multiline(full_prompt, esc_prompt, 6400);
    const char *think_cfg = ",\"thinkingConfig\":{\"thinkingLevel\":\"medium\"}";

    gem_http_ctx_t hctx = { .buf = resp_buf, .cap = 16384, .len = 0, .tls_err = 0, .mbed_err = 0, .sock_err = 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 35000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .cert_pem = GOOGLE_ROOT_CA_PEM,
        .event_handler = gemini_http_evt_cb,
        .user_data = &hctx,
    };

    esp_err_t perf_err = ESP_FAIL;
    int status = 0;
    for (int try_idx = 0; try_idx < 2; try_idx++) {
        const char *active_think = (try_idx == 0) ? think_cfg : "";
        int req_len = snprintf(req_json, 6900,
                               "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"%s\"}]}],"
                               "\"generationConfig\":{\"temperature\":0.25,\"maxOutputTokens\":4096%s}}",
                               esc_prompt, active_think);
        hctx.len = 0;
        hctx.tls_err = 0;
        hctx.mbed_err = 0;
        hctx.sock_err = 0;
        resp_buf[0] = '\0';

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            free(esc_prompt); free(req_json); free(resp_buf);
            snprintf(out_reply, out_reply_sz, "Eroare initializare HTTPS Gemini.");
            return -1;
        }
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "x-goog-api-key", api_key);
        esp_http_client_set_post_field(client, req_json, req_len);

        perf_err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (perf_err == ESP_ERR_HTTP_EAGAIN) {
            ESP_LOGW(TAG, "Gemini HTTPS EAGAIN (%s), retrying...", model);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (perf_err == ESP_OK && status == 400 && try_idx == 0 && think_cfg[0] != '\0') {
            ESP_LOGW(TAG, "Gemini HTTP 400 with thinkingConfig (%s), retrying without...", model);
            continue;
        }
        break;
    }

    if (perf_err != ESP_OK) {
        ESP_LOGE(TAG, "Gemini HTTPS perform failed (%s): %s tls=0x%x mbed=-0x%x flags=0x%x",
                 model, esp_err_to_name(perf_err), hctx.tls_err, -hctx.mbed_err, hctx.sock_err);
        snprintf(out_reply, out_reply_sz,
                 "Nu s-a putut deschide conexiunea HTTPS cu Google Gemini (%s | tls=0x%x | mbed=-0x%x).",
                 esp_err_to_name(perf_err), hctx.tls_err, -hctx.mbed_err);
        free(esc_prompt); free(req_json); free(resp_buf);
        return -1;
    }

    if (status == 200) {
        if (!gemini_extract_text(resp_buf, out_reply, out_reply_sz)) {
            snprintf(out_reply, out_reply_sz, "Raspuns gol sau invalid de la modelul %s.", model);
            status = 500;
        }
    } else {
        char err_msg[220] = {0};
        json_get_str(resp_buf, "message", err_msg, sizeof(err_msg));
        snprintf(out_reply, out_reply_sz, "Eroare Gemini HTTP %d (%s): %s",
                 status, model, err_msg[0] ? err_msg : "verifica cheia API / modelul");
    }

    free(esc_prompt);
    free(req_json);
    free(resp_buf);
    return status;
}

static bool gemini_call_with_fallback(const char *full_prompt,
                                      char *out_reply, size_t out_reply_sz,
                                      char *out_used_model, size_t out_model_sz) {
    if (strlen(g_settings.gemini_api_key) < 8) {
        snprintf(out_reply, out_reply_sz,
                 "Cheia API Gemini nu este configurata. Deschide tabul Setari si introdu cheia API Google AI Studio.");
        return false;
    }

    const char *pref = g_settings.gemini_model[0] ? g_settings.gemini_model : "gemini-flash-lite-latest";
    const char *candidates[5] = {
        pref,
        "gemini-flash-lite-latest",
        "gemini-3.5-flash-lite",
        "gemini-3.8-flash",
        "gemini-4-hydrogen"
    };

    for (int i = 0; i < 5; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(candidates[i], candidates[j]) == 0) { dup = true; break; }
        }
        if (dup) continue;

        int st = gemini_call_single(g_settings.gemini_api_key, candidates[i],
                                    full_prompt, out_reply, out_reply_sz);
        if (st == 200) {
            if (out_used_model && out_model_sz > 0) {
                snprintf(out_used_model, out_model_sz, "%s", candidates[i]);
            }
            return true;
        }
        // Fallback to next model if deprecated/not found (404 / 400), overloaded (503 / 429), or transient timeout (-1)
        if (st > 0 && st != 404 && st != 400 && st != 429 && st != 503) {
            return false;
        }
    }

    // Dynamic auto-discovery from GET /v1beta/models if all known names were replaced in the future
    char disc_model[48] = {0};
    if (gemini_discover_flash_model(g_settings.gemini_api_key, disc_model, sizeof(disc_model))) {
        int st = gemini_call_single(g_settings.gemini_api_key, disc_model,
                                    full_prompt, out_reply, out_reply_sz);
        if (st == 200) {
            if (out_used_model && out_model_sz > 0) {
                snprintf(out_used_model, out_model_sz, "%s", disc_model);
            }
            return true;
        }
    }
    return false;
}

static esp_err_t ai_ask_post_handler(httpd_req_t *req) {
    char *body = ai_alloc_buf(1536);
    char *ctx = ai_alloc_buf(3200);
    char *prompt = ai_alloc_buf(5000);
    char *reply = ai_alloc_buf(3072);
    char *esc_reply = ai_alloc_buf(3600);
    char *resp_json = ai_alloc_buf(3900);
    if (!body || !ctx || !prompt || !reply || !esc_reply || !resp_json) {
        free(body); free(ctx); free(prompt); free(reply); free(esc_reply); free(resp_json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Memorie insuficienta\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (read_req_body(req, body, 1535) != ESP_OK) {
        free(body); free(ctx); free(prompt); free(reply); free(esc_reply); free(resp_json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Cerere invalida\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char question[700] = {0};
    char bat_info[200] = {0};
    json_get_str(body, "question", question, sizeof(question));
    json_get_str(body, "bat_info", bat_info, sizeof(bat_info));
    if (question[0] == '\0') {
        strncpy(question, "Cum functioneaza sistemul solar si bateria acum?", sizeof(question) - 1);
    }

    build_inverter_ai_context(bat_info, ctx, 3200);
    snprintf(prompt, 5000,
             "Ești Argus, asistentul AI Solar al aplicației Deye Smart Energy (funcție strict READ-ONLY: doar citești, interpretezi și explici, NU modifici niciodată setări în invertor). "
             "Răspunde direct la întrebare în limba română corectă, obligatoriu cu diacritice (ă, â, î, ș, ț), clar, pe înțelesul oricui și concis (fără simboluri markdown ** sau `). "
             "IMPORTANT: NU te saluta și NU te prezenta din nou (nu începe cu 'Salut! Sunt Argus...') decât dacă utilizatorul doar te salută. "
             "Dacă utilizatorul îți cere explicit să trimiți informarea/răspunsul pe Telegram, adaugă exact la sfârșitul răspunsului tag-ul [SEND_TELEGRAM] (serverul ESP32-S3 îl va trimite automat pe Telegram). "
             "ESP32-S3 comunică cu loggerul Solarman prin Wi-Fi / LAN (TCP port 8899), iar setările bateriei (capacitate, rezervă autonomie, Mod Vară) sunt salvate local în NVS. "
             "Folosește strict datele reale de mai jos (dacă invertorul este DECONECTAT și istoricul e 0, spune clar acest lucru):\n\n"
             "%s\n"
             "ÎNTREBARE UTILIZATOR: %s",
             ctx, question);

    char used_model[48] = {0};
    deye_modbus_set_scan_paused(true);
    vTaskDelay(pdMS_TO_TICKS(120));
    bool ok = gemini_call_with_fallback(prompt, reply, 3072, used_model, sizeof(used_model));
    deye_modbus_set_scan_paused(false);
    if (ok) {
        char *tg_tag = strstr(reply, "[SEND_TELEGRAM]");
        bool want_tg = (tg_tag != NULL);
        if (tg_tag) {
            *tg_tag = '\0';
            size_t rlen = strlen(reply);
            while (rlen > 0 && (reply[rlen - 1] == '\n' || reply[rlen - 1] == '\r' || reply[rlen - 1] == ' ')) {
                reply[--rlen] = '\0';
            }
        } else {
            char qlow[700];
            size_t qi = 0;
            for (; question[qi] && qi < sizeof(qlow) - 1; qi++) qlow[qi] = (char)tolower((unsigned char)question[qi]);
            qlow[qi] = '\0';
            if (strstr(qlow, "telegram") && (strstr(qlow, "trimit") || strstr(qlow, "send") || strstr(qlow, "expediaz"))) {
                want_tg = true;
            }
        }
        if (want_tg) {
            char *tg_msg = ai_alloc_buf(3300);
            bool tg_sent = false;
            if (tg_msg) {
                snprintf(tg_msg, 3300, "\xF0\x9F\xA4\x96 ARGUS (Deye Smart Energy)\n\n%s", reply);
                tg_sent = (telegram_notify_send(tg_msg) == ESP_OK);
                free(tg_msg);
            }
            size_t rlen = strlen(reply);
            snprintf(reply + rlen, 3072 - rlen, "%s",
                     tg_sent ? "\n\n\xE2\x9C\x85 Trimis pe Telegram!"
                             : "\n\n\xE2\x9A\xA0\xEF\xB8\x8F Nu s-a putut trimite pe Telegram (verific\xC4\x83 Token & Chat ID \xC3\xAEn Set\xC4\x83ri).");
        }
    }
    json_escape_multiline(reply, esc_reply, 3600);
    if (ok) {
        snprintf(resp_json, 3900, "{\"ok\":true,\"answer\":\"%s\",\"model\":\"%s\"}", esc_reply, used_model);
    } else {
        snprintf(resp_json, 3900, "{\"ok\":false,\"error\":\"%s\"}", esc_reply);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp_json, HTTPD_RESP_USE_STRLEN);

    free(body); free(ctx); free(prompt); free(reply); free(esc_reply); free(resp_json);
    return ESP_OK;
}

static esp_err_t ai_audit_post_handler(httpd_req_t *req) {
    char *body = ai_alloc_buf(512);
    char *ctx = ai_alloc_buf(3200);
    char *prompt = ai_alloc_buf(5000);
    char *reply = ai_alloc_buf(3072);
    char *esc_reply = ai_alloc_buf(3600);
    char *resp_json = ai_alloc_buf(3900);
    if (!body || !ctx || !prompt || !reply || !esc_reply || !resp_json) {
        free(body); free(ctx); free(prompt); free(reply); free(esc_reply); free(resp_json);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Memorie insuficienta\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char bat_info[200] = {0};
    if (req->content_len > 0 && read_req_body(req, body, 511) == ESP_OK) {
        json_get_str(body, "bat_info", bat_info, sizeof(bat_info));
    }

    build_inverter_ai_context(bat_info, ctx, 3200);
    snprintf(prompt, 5000,
             "Ești Argus, asistentul AI Solar din Deye Smart Energy (funcție strict READ-ONLY). "
             "Realizează un AUDIT SCURT ȘI CLAR al sistemului pentru proprietar, în limba română corectă, obligatoriu cu diacritice (ă, â, î, ș, ț), fără simboluri markdown ** sau `, folosind puncte •. "
             "IMPORTANT: NU te prezenta și NU saluta (începe direct cu punctul 1). "
             "Reține că ESP32-S3 comunică cu loggerul Solarman prin Wi-Fi / LAN (TCP port 8899), iar capacitatea bateriei, rezerva de autonomie și Mod Vară sunt setări locale salvate pe ESP32-S3 (raportează starea lor reală chiar dacă invertorul e offline):\n"
             "1. Bilanț energie (azi, lună, an, autoconsum)\n"
             "2. Sănătate & autonomie baterie (SoC, tensiune, temperatură, setări locale & Mod Vară)\n"
             "3. Calitate rețea EN 50549-1 (tensiune, frecvență, export/import)\n"
             "4. Concluzie (OK / ATENȚIE / CRITIC) și recomandări (dacă e offline, verifică conexiunea Wi-Fi, IP-ul loggerului în Setări și alimentarea invertorului).\n\n%s",
             ctx);

    char used_model[48] = {0};
    deye_modbus_set_scan_paused(true);
    vTaskDelay(pdMS_TO_TICKS(120));
    bool ok = gemini_call_with_fallback(prompt, reply, 3072, used_model, sizeof(used_model));
    deye_modbus_set_scan_paused(false);
    bool tg_ok = false;
    if (ok) {
        char *tg_msg = malloc(3300);
        if (tg_msg) {
            snprintf(tg_msg, 3300, "🤖 AUDIT AI SOLAR (Deye Smart Energy)\n\n%s", reply);
            tg_ok = (telegram_notify_send(tg_msg) == ESP_OK);
            free(tg_msg);
        }
    }

    json_escape_multiline(reply, esc_reply, 3600);
    if (ok) {
        snprintf(resp_json, 3900,
                 "{\"ok\":true,\"answer\":\"%s\",\"model\":\"%s\",\"tg_sent\":%s}",
                 esc_reply, used_model, tg_ok ? "true" : "false");
    } else {
        snprintf(resp_json, 3900, "{\"ok\":false,\"error\":\"%s\"}", esc_reply);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, resp_json, HTTPD_RESP_USE_STRLEN);

    free(body); free(ctx); free(prompt); free(reply); free(esc_reply); free(resp_json);
    return ESP_OK;
}

static void ac_state_to_cjson(cJSON *parent, const ac_state_t *st) {
    cJSON *s = cJSON_AddObjectToObject(parent, "state");
    cJSON_AddBoolToObject(s, "power", st->power);
    cJSON_AddNumberToObject(s, "target_temp", st->target_temp);
    if (st->has_current_temp) {
        cJSON_AddNumberToObject(s, "current_temp", ((int)(st->current_temp * 10.0f + 0.5f)) / 10.0);
    } else {
        cJSON_AddNullToObject(s, "current_temp");
    }
    cJSON_AddStringToObject(s, "mode", st->mode);
    cJSON_AddNumberToObject(s, "fan_speed", st->fan_speed);
    cJSON_AddNumberToObject(s, "fan_max", st->fan_max > 0 ? st->fan_max : 7);
}

static esp_err_t ac_status_get_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "brand", g_settings.ac_brand);
    cJSON_AddStringToObject(root, "tcl_email", g_settings.ac_tcl_email);
    cJSON_AddBoolToObject(root, "tcl_password_set", g_settings.ac_tcl_password[0] != '\0');
    cJSON_AddStringToObject(root, "tuya_region", g_settings.ac_tuya_region[0] ? g_settings.ac_tuya_region : "eu");
    cJSON_AddStringToObject(root, "tuya_client_id", g_settings.ac_tuya_client_id);
    cJSON_AddBoolToObject(root, "tuya_secret_set", g_settings.ac_tuya_client_secret[0] != '\0');
    cJSON_AddStringToObject(root, "tuya_device_id", g_settings.ac_tuya_device_id);
    cJSON_AddStringToObject(root, "selected_device_id", g_settings.ac_selected_device_id);

    cJSON *devs = cJSON_Parse(g_settings.ac_devices_json[0] ? g_settings.ac_devices_json : "[]");
    cJSON_AddItemToObject(root, "devices", devs ? devs : cJSON_CreateArray());

    ac_state_t st = {0};
    char err_buf[128] = {0};
    bool connected = false;
    if (g_settings.ac_brand[0] != '\0' && g_settings.ac_selected_device_id[0] != '\0') {
        connected = ac_control_get_state(&st, err_buf, sizeof(err_buf));
    }
    cJSON_AddBoolToObject(root, "connected", connected);
    if (connected) {
        ac_state_to_cjson(root, &st);
        cJSON_AddStringToObject(root, "error", "");
    } else {
        cJSON_AddNullToObject(root, "state");
        cJSON_AddStringToObject(root, "error", err_buf);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, out ? out : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t ac_config_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    char body[512];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    cJSON *in = cJSON_Parse(body);
    if (!in) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"JSON invalid\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *disc = cJSON_GetObjectItem(in, "disconnect");
    if (cJSON_IsTrue(disc)) {
        app_settings_t cfg = g_settings;
        cfg.ac_brand[0] = '\0';
        cfg.ac_tcl_password[0] = '\0';
        cfg.ac_tuya_client_secret[0] = '\0';
        cfg.ac_selected_device_id[0] = '\0';
        strcpy(cfg.ac_devices_json, "[]");
        app_settings_save(&cfg);
        ac_control_clear_cache();
        cJSON_Delete(in);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"connected\":false,\"devices\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *b_j = cJSON_GetObjectItem(in, "brand");
    const char *brand = (cJSON_IsString(b_j) && b_j->valuestring) ? b_j->valuestring : "";

    if (strcmp(brand, "tcl") == 0) {
        cJSON *em_j = cJSON_GetObjectItem(in, "tcl_email");
        cJSON *pw_j = cJSON_GetObjectItem(in, "tcl_password");
        char email[96] = {0}, pw[64] = {0};
        if (cJSON_IsString(em_j) && em_j->valuestring[0]) strncpy(email, em_j->valuestring, sizeof(email) - 1);
        else strncpy(email, g_settings.ac_tcl_email, sizeof(email) - 1);
        if (cJSON_IsString(pw_j) && pw_j->valuestring[0]) strncpy(pw, pw_j->valuestring, sizeof(pw) - 1);
        else strncpy(pw, g_settings.ac_tcl_password, sizeof(pw) - 1);
        cJSON_Delete(in);

        if (!email[0] || !pw[0]) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Introdu e-mailul si parola TCL Home.\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }

        char devs_json[512] = "[]", first_id[48] = {0}, err_buf[128] = {0};
        if (!ac_control_connect_tcl(email, pw, devs_json, sizeof(devs_json), first_id, sizeof(first_id), err_buf, sizeof(err_buf))) {
            cJSON *er = cJSON_CreateObject();
            cJSON_AddBoolToObject(er, "ok", false);
            cJSON_AddStringToObject(er, "error", err_buf[0] ? err_buf : "Conectare TCL esuata");
            char *out = cJSON_PrintUnformatted(er);
            cJSON_Delete(er);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
            free(out);
            return ESP_OK;
        }

        app_settings_t cfg = g_settings;
        strcpy(cfg.ac_brand, "tcl");
        strncpy(cfg.ac_tcl_email, email, sizeof(cfg.ac_tcl_email) - 1);
        strncpy(cfg.ac_tcl_password, pw, sizeof(cfg.ac_tcl_password) - 1);
        strncpy(cfg.ac_devices_json, devs_json, sizeof(cfg.ac_devices_json) - 1);
        strncpy(cfg.ac_selected_device_id, first_id, sizeof(cfg.ac_selected_device_id) - 1);
        app_settings_save(&cfg);

        ac_state_t st = {0};
        bool ok_st = first_id[0] ? ac_control_get_state(&st, err_buf, sizeof(err_buf)) : false;
        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "ok", true);
        cJSON_AddStringToObject(res, "brand", "tcl");
        cJSON_AddBoolToObject(res, "connected", first_id[0] != '\0');
        cJSON_AddStringToObject(res, "selected_device_id", first_id);
        cJSON *dj = cJSON_Parse(devs_json);
        cJSON_AddItemToObject(res, "devices", dj ? dj : cJSON_CreateArray());
        if (ok_st) ac_state_to_cjson(res, &st);
        else cJSON_AddNullToObject(res, "state");

        char *out = cJSON_PrintUnformatted(res);
        cJSON_Delete(res);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        free(out);
        return ESP_OK;
    }

    if (strcmp(brand, "tuya") == 0) {
        cJSON *rg_j = cJSON_GetObjectItem(in, "tuya_region");
        cJSON *id_j = cJSON_GetObjectItem(in, "tuya_client_id");
        cJSON *sc_j = cJSON_GetObjectItem(in, "tuya_client_secret");
        cJSON *dv_j = cJSON_GetObjectItem(in, "tuya_device_id");

        app_settings_t cfg = g_settings;
        strcpy(cfg.ac_brand, "tuya");
        strncpy(cfg.ac_tuya_region, (cJSON_IsString(rg_j) && rg_j->valuestring[0]) ? rg_j->valuestring : "eu", sizeof(cfg.ac_tuya_region) - 1);
        if (cJSON_IsString(id_j) && id_j->valuestring[0]) strncpy(cfg.ac_tuya_client_id, id_j->valuestring, sizeof(cfg.ac_tuya_client_id) - 1);
        if (cJSON_IsString(sc_j) && sc_j->valuestring[0]) strncpy(cfg.ac_tuya_client_secret, sc_j->valuestring, sizeof(cfg.ac_tuya_client_secret) - 1);
        if (cJSON_IsString(dv_j) && dv_j->valuestring[0]) strncpy(cfg.ac_tuya_device_id, dv_j->valuestring, sizeof(cfg.ac_tuya_device_id) - 1);
        cJSON_Delete(in);

        if (!cfg.ac_tuya_client_id[0] || !cfg.ac_tuya_client_secret[0] || !cfg.ac_tuya_device_id[0]) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"Introdu Client ID, Secret si Device ID Tuya.\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        strncpy(cfg.ac_selected_device_id, cfg.ac_tuya_device_id, sizeof(cfg.ac_selected_device_id) - 1);
        snprintf(cfg.ac_devices_json, sizeof(cfg.ac_devices_json),
                 "[{\"id\":\"%s\",\"name\":\"AC Tuya\",\"online\":true}]", cfg.ac_tuya_device_id);
        app_settings_save(&cfg);

        ac_state_t st = {0};
        char err_buf[128] = {0};
        if (!ac_control_get_state(&st, err_buf, sizeof(err_buf))) {
            cJSON *er = cJSON_CreateObject();
            cJSON_AddBoolToObject(er, "ok", false);
            cJSON_AddStringToObject(er, "error", err_buf[0] ? err_buf : "Eroare Tuya");
            char *out = cJSON_PrintUnformatted(er);
            cJSON_Delete(er);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
            free(out);
            return ESP_OK;
        }

        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "ok", true);
        cJSON_AddStringToObject(res, "brand", "tuya");
        cJSON_AddBoolToObject(res, "connected", true);
        cJSON_AddStringToObject(res, "selected_device_id", cfg.ac_selected_device_id);
        cJSON *dj = cJSON_Parse(cfg.ac_devices_json);
        cJSON_AddItemToObject(res, "devices", dj ? dj : cJSON_CreateArray());
        ac_state_to_cjson(res, &st);
        char *out = cJSON_PrintUnformatted(res);
        cJSON_Delete(res);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
        free(out);
        return ESP_OK;
    }

    cJSON_Delete(in);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":false,\"error\":\"Selecteaza un brand valid (TCL sau Tuya).\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ac_control_post_handler(httpd_req_t *req) {
    if (!check_admin_pin_hdr(req)) return ESP_OK;

    char body[256];
    if (read_req_body(req, body, sizeof(body)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"ok\":false,\"error\":\"Body invalid\"}");
        return ESP_FAIL;
    }

    cJSON *in = cJSON_Parse(body);
    if (!in) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"JSON invalid\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    cJSON *did_j = cJSON_GetObjectItem(in, "device_id");
    if (cJSON_IsString(did_j) && did_j->valuestring[0] &&
        strcmp(did_j->valuestring, g_settings.ac_selected_device_id) != 0) {
        app_settings_t cfg = g_settings;
        strncpy(cfg.ac_selected_device_id, did_j->valuestring, sizeof(cfg.ac_selected_device_id) - 1);
        app_settings_save(&cfg);
    }

    ac_cmd_t cmd = {0};
    cJSON *p_j = cJSON_GetObjectItem(in, "power");
    cJSON *t_j = cJSON_GetObjectItem(in, "target_temp");
    cJSON *m_j = cJSON_GetObjectItem(in, "mode");
    cJSON *f_j = cJSON_GetObjectItem(in, "fan_speed");
    if (cJSON_IsBool(p_j)) { cmd.has_power = true; cmd.power = cJSON_IsTrue(p_j); }
    if (cJSON_IsNumber(t_j)) { cmd.has_target_temp = true; cmd.target_temp = t_j->valueint; }
    if (cJSON_IsString(m_j) && m_j->valuestring[0]) { cmd.has_mode = true; strncpy(cmd.mode, m_j->valuestring, sizeof(cmd.mode) - 1); }
    if (cJSON_IsNumber(f_j)) { cmd.has_fan_speed = true; cmd.fan_speed = f_j->valueint; }
    cJSON_Delete(in);

    ac_state_t st = {0};
    char err_buf[128] = {0};
    bool ok = false;
    if (cmd.has_power || cmd.has_target_temp || cmd.has_mode || cmd.has_fan_speed) {
        ok = ac_control_send_cmd(&cmd, &st, err_buf, sizeof(err_buf));
    } else {
        ok = ac_control_get_state(&st, err_buf, sizeof(err_buf));
    }

    cJSON *res = cJSON_CreateObject();
    cJSON_AddBoolToObject(res, "ok", ok);
    if (ok) {
        ac_state_to_cjson(res, &st);
    } else {
        cJSON_AddStringToObject(res, "error", err_buf[0] ? err_buf : "Eroare comanda AC");
    }
    char *out = cJSON_PrintUnformatted(res);
    cJSON_Delete(res);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    free(out);
    return ESP_OK;
}

static esp_err_t manifest_get_handler(httpd_req_t *req) {
    static const char MANIFEST_JSON[] =
        "{"
        "\"name\":\"Deye Smart Energy\","
        "\"short_name\":\"Deye Energy\","
        "\"start_url\":\"/\","
        "\"scope\":\"/\","
        "\"display\":\"standalone\","
        "\"orientation\":\"portrait\","
        "\"background_color\":\"#070d19\","
        "\"theme_color\":\"#070d19\","
        "\"icons\":["
        "{\"src\":\"/icon-192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"},"
        "{\"src\":\"/icon-512.png\",\"sizes\":\"512x512\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}"
        "]"
        "}";
    httpd_resp_set_type(req, "application/manifest+json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, MANIFEST_JSON, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t sw_get_handler(httpd_req_t *req) {
    static const char SW_JS[] =
        "self.addEventListener('install', () => self.skipWaiting());\n"
        "self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));\n"
        "self.addEventListener('fetch', (e) => { e.respondWith(fetch(e.request)); });\n";
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SW_JS, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t icon_192_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    return httpd_resp_send(req, (const char *)icon_192_png_start, (ssize_t)(icon_192_png_end - icon_192_png_start));
}

static esp_err_t icon_512_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=604800");
    return httpd_resp_send(req, (const char *)icon_512_png_start, (ssize_t)(icon_512_png_end - icon_512_png_start));
}

esp_err_t local_web_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 36;
    config.max_open_sockets = 8;
    config.backlog_conn = 8;
    config.lru_purge_enable = true;
    config.stack_size = 16384;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Nu s-a putut porni serverul HTTP: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                      .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/manifest.json",         .method = HTTP_GET,  .handler = manifest_get_handler },
        { .uri = "/sw.js",                 .method = HTTP_GET,  .handler = sw_get_handler },
        { .uri = "/icon-192.png",          .method = HTTP_GET,  .handler = icon_192_get_handler },
        { .uri = "/icon-512.png",          .method = HTTP_GET,  .handler = icon_512_get_handler },
        { .uri = "/setup",                 .method = HTTP_GET,  .handler = setup_get_handler },
        { .uri = "/scan",                  .method = HTTP_GET,  .handler = scan_get_handler },
        { .uri = "/save",                  .method = HTTP_POST, .handler = save_post_handler },
        { .uri = "/api/telemetry",         .method = HTTP_GET,  .handler = telemetry_get_handler },
        { .uri = "/api/history",           .method = HTTP_GET,  .handler = history_get_handler },
        { .uri = "/api/chart",             .method = HTTP_GET,  .handler = chart_get_handler },
        { .uri = "/api/chart/fasttest",    .method = HTTP_POST, .handler = chart_fasttest_post_handler },
        { .uri = "/api/history/clear",     .method = HTTP_POST, .handler = history_clear_post_handler },
        { .uri = "/api/config",            .method = HTTP_GET,  .handler = config_get_handler },
        { .uri = "/api/config",            .method = HTTP_POST, .handler = config_post_handler },
        { .uri = "/api/battery/config",    .method = HTTP_POST, .handler = battery_config_post_handler },
        { .uri = "/api/ac/status",         .method = HTTP_GET,  .handler = ac_status_get_handler },
        { .uri = "/api/ac/config",         .method = HTTP_POST, .handler = ac_config_post_handler },
        { .uri = "/api/ac/control",        .method = HTTP_POST, .handler = ac_control_post_handler },
        { .uri = "/api/telegram/test",     .method = HTTP_POST, .handler = telegram_test_post_handler },
        { .uri = "/api/logger/scan",       .method = HTTP_GET,  .handler = logger_scan_get_handler },
        { .uri = "/api/logger/scan/status",.method = HTTP_GET,  .handler = logger_scan_status_get_handler },
        { .uri = "/api/ota",               .method = HTTP_POST, .handler = ota_post_handler },
        { .uri = "/api/ota/check",         .method = HTTP_GET,  .handler = ota_check_get_handler },
        { .uri = "/api/ota/github",        .method = HTTP_POST, .handler = ota_github_post_handler },
        { .uri = "/api/ota/status",        .method = HTTP_GET,  .handler = ota_status_get_handler },
        { .uri = "/api/pin/verify",        .method = HTTP_POST, .handler = pin_verify_post_handler },
        { .uri = "/api/ai/ask",            .method = HTTP_POST, .handler = ai_ask_post_handler },
        { .uri = "/api/ai/audit",          .method = HTTP_POST, .handler = ai_audit_post_handler },
        { .uri = "/api/reboot",            .method = HTTP_POST, .handler = reboot_post_handler },
        { .uri = "/*",                     .method = HTTP_GET,  .handler = captive_redirect_handler },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Server web local + Captive Portal pornit (http://deye.local/)");
    return ESP_OK;
}
