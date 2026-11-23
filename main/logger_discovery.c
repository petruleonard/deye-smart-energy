#include "logger_discovery.h"
#include "deye_modbus.h"
#include "solarman_v5.h"
#include "app_settings.h"
#include "deye_data.h"
#include "wifi_manager.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "logger_disc";
#define SOLARMAN_DISC_PORT 48899
#define SOLARMAN_TCP_PORT  8899
#define SOLARMAN_HTTP_PORT 80

static const char *DISC_CMDS[] = {
    "WIFIKIT-214028-READ",
    "HF-A11ASSISTHREAD",
};

static void normalize_mac(const char *src, char *dst, size_t dst_sz)
{
    size_t j = 0;
    if (!src || dst_sz == 0) {
        if (dst_sz > 0) dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c)) {
            dst[j++] = (char)toupper(c);
        }
    }
    dst[j] = '\0';
}

static bool parse_disc_reply(char *rx_buf, const struct sockaddr_in *src_addr,
                             char *out_ip, size_t out_ip_sz,
                             char *out_mac, size_t out_mac_sz, uint32_t *out_sn)
{
    char *p_ip = rx_buf;
    char *c1 = strchr(p_ip, ',');
    if (!c1) return false;
    *c1 = '\0';

    char *p_mac = c1 + 1;
    char *c2 = strchr(p_mac, ',');
    if (!c2) return false;
    *c2 = '\0';

    char *p_sn = c2 + 1;
    for (int k = (int)strlen(p_sn) - 1; k >= 0; k--) {
        if (p_sn[k] == '\r' || p_sn[k] == '\n' || p_sn[k] == ' ') p_sn[k] = '\0';
        else break;
    }

    if (strlen(p_ip) < 7 || strcmp(p_ip, "0.0.0.0") == 0) {
        ip4addr_ntoa_r((const ip4_addr_t *)&src_addr->sin_addr, out_ip, (int)out_ip_sz);
    } else {
        strncpy(out_ip, p_ip, out_ip_sz - 1);
        out_ip[out_ip_sz - 1] = '\0';
    }

    normalize_mac(p_mac, out_mac, out_mac_sz);
    *out_sn = (uint32_t)strtoul(p_sn, NULL, 10);
    return (*out_sn > 0 || strlen(out_mac) == 12);
}

static void add_discovered_unique(discovered_logger_t *list, int *count, int max_count,
                                  const char *ip, const char *mac, uint32_t sn,
                                  const char *method_tag, char tags[][24])
{
    if (*count >= max_count) return;
    for (int i = 0; i < *count; i++) {
        if (strcmp(list[i].ip, ip) == 0 && (sn == 0 || list[i].sn == sn)) {
            return;
        }
        if (mac[0] != '\0' && strcmp(list[i].mac, mac) == 0 && list[i].sn == sn) {
            return;
        }
    }
    memset(&list[*count], 0, sizeof(discovered_logger_t));
    strncpy(list[*count].ip, ip, sizeof(list[*count].ip) - 1);
    strncpy(list[*count].mac, mac, sizeof(list[*count].mac) - 1);
    list[*count].sn = sn;
    if (tags && method_tag) {
        snprintf(tags[*count], 24, "%s", method_tag);
    }
    (*count)++;
}

static void udp_broadcast_discover(const esp_netif_ip_info_t *sta_ip,
                                   discovered_logger_t *list, int *count, int max_count,
                                   char tags[][24])
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    int broadcast = 1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 350000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(0);
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    struct sockaddr_in bcast_sub = {0};
    bcast_sub.sin_family = AF_INET;
    bcast_sub.sin_port = htons(SOLARMAN_DISC_PORT);
    bool has_sub_bcast = (sta_ip && sta_ip->ip.addr != 0 && sta_ip->netmask.addr != 0);
    if (has_sub_bcast) {
        bcast_sub.sin_addr.s_addr = (sta_ip->ip.addr & sta_ip->netmask.addr) | (~sta_ip->netmask.addr);
    }

    struct sockaddr_in bcast_global = {0};
    bcast_global.sin_family = AF_INET;
    bcast_global.sin_port = htons(SOLARMAN_DISC_PORT);
    bcast_global.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    for (int round = 0; round < 2; round++) {
        const char *cmd = DISC_CMDS[round];
        size_t cmd_len = strlen(cmd);
        if (has_sub_bcast) {
            sendto(sock, cmd, cmd_len, 0, (struct sockaddr *)&bcast_sub, sizeof(bcast_sub));
        }
        sendto(sock, cmd, cmd_len, 0, (struct sockaddr *)&bcast_global, sizeof(bcast_global));

        char rx_buf[160];
        while (*count < max_count) {
            struct sockaddr_in src_addr = {0};
            socklen_t addr_len = sizeof(src_addr);
            int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                               (struct sockaddr *)&src_addr, &addr_len);
            if (len <= 0) break;
            rx_buf[len] = '\0';

            char ip[16] = {0}, mac[20] = {0};
            uint32_t sn = 0;
            if (parse_disc_reply(rx_buf, &src_addr, ip, sizeof(ip), mac, sizeof(mac), &sn)) {
                ESP_LOGI(TAG, "UDP 48899 discovered: IP=%s, MAC=%s, SN=%lu", ip, mac, (unsigned long)sn);
                add_discovered_unique(list, count, max_count, ip, mac, sn, "UDP", tags);
            }
        }
        if (*count > 0 && round >= 1) break;
    }

    close(sock);
}

static bool udp_unicast_query(const char *target_ip, char *out_mac, size_t out_mac_sz, uint32_t *out_sn)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 350000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SOLARMAN_DISC_PORT);
    if (inet_pton(AF_INET, target_ip, &dst.sin_addr) != 1) {
        close(sock);
        return false;
    }

    for (int attempt = 0; attempt < 1; attempt++) {
        const char *cmd = DISC_CMDS[attempt];
        sendto(sock, cmd, strlen(cmd), 0, (struct sockaddr *)&dst, sizeof(dst));

        char rx_buf[160];
        struct sockaddr_in src_addr = {0};
        socklen_t addr_len = sizeof(src_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&src_addr, &addr_len);
        if (len > 0) {
            rx_buf[len] = '\0';
            char ip[16] = {0};
            if (parse_disc_reply(rx_buf, &src_addr, ip, sizeof(ip), out_mac, out_mac_sz, out_sn)) {
                close(sock);
                return true;
            }
        }
    }
    close(sock);
    return false;
}

static bool http80_query_sn(const char *target_ip, uint32_t *out_sn)
{
    if (!target_ip || !out_sn) return false;
    *out_sn = 0;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return false;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 350000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(SOLARMAN_HTTP_PORT);
    if (inet_pton(AF_INET, target_ip, &dst.sin_addr) != 1) {
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        close(sock);
        return false;
    }

    char req[220];
    int req_len = snprintf(req, sizeof(req),
                           "GET /status.html HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "Authorization: Basic YWRtaW46YWRtaW4=\r\n"
                           "Connection: close\r\n\r\n",
                           target_ip);
    if (send(sock, req, (size_t)req_len, 0) <= 0) {
        close(sock);
        return false;
    }

    char *buf = malloc(2049);
    if (!buf) {
        close(sock);
        return false;
    }
    size_t total = 0;
    while (total < 2048) {
        int r = recv(sock, buf + total, 2048 - total, 0);
        if (r <= 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';
    close(sock);

    char *p = strstr(buf, "webdata_sn");
    if (p) {
        p += strlen("webdata_sn");
        while (*p && !isdigit((unsigned char)*p) && *p != '\n' && *p != ';') p++;
        if (isdigit((unsigned char)*p)) {
            uint32_t sn = (uint32_t)strtoul(p, NULL, 10);
            if (sn > 100000UL) {
                *out_sn = sn;
                ESP_LOGI(TAG, "HTTP :80 status.html on %s -> SN=%lu", target_ip, (unsigned long)sn);
                free(buf);
                return true;
            }
        }
    }
    free(buf);
    return false;
}

typedef struct {
    bool scanned;
    uint32_t sns[8];
    int8_t rssi[8];
    int count;
} ap_sn_cache_t;

static void ap_sn_ensure_scanned(ap_sn_cache_t *cache)
{
    if (cache->scanned) return;
    cache->scanned = true;
    cache->count = 0;

    wifi_ap_record_t *aps = calloc(20, sizeof(wifi_ap_record_t));
    if (!aps) return;

    uint16_t max_aps = 20;
    if (wifi_manager_scan(aps, &max_aps) == ESP_OK) {
        for (uint16_t i = 0; i < max_aps; i++) {
            const char *ssid = (const char *)aps[i].ssid;
            if (strncmp(ssid, "AP_", 3) == 0 && isdigit((unsigned char)ssid[3])) {
                const char *d = ssid + 3;
                bool all_digits = true;
                size_t dlen = strlen(d);
                if (dlen < 7 || dlen > 10) all_digits = false;
                for (size_t k = 0; k < dlen && all_digits; k++) {
                    if (!isdigit((unsigned char)d[k])) all_digits = false;
                }
                if (all_digits) {
                    uint32_t sn = (uint32_t)strtoul(d, NULL, 10);
                    if (sn > 100000UL && cache->count < 8) {
                        bool dup = false;
                        for (int j = 0; j < cache->count; j++) {
                            if (cache->sns[j] == sn) { dup = true; break; }
                        }
                        if (!dup) {
                            cache->sns[cache->count] = sn;
                            cache->rssi[cache->count] = aps[i].rssi;
                            cache->count++;
                        }
                    }
                }
            }
        }
        for (int i = 0; i < cache->count - 1; i++) {
            for (int j = i + 1; j < cache->count; j++) {
                if (cache->rssi[j] > cache->rssi[i]) {
                    uint32_t ts = cache->sns[i]; cache->sns[i] = cache->sns[j]; cache->sns[j] = ts;
                    int8_t tr = cache->rssi[i]; cache->rssi[i] = cache->rssi[j]; cache->rssi[j] = tr;
                }
            }
        }
        for (int i = 0; i < cache->count; i++) {
            ESP_LOGI(TAG, "Wi-Fi AP candidate #%d: AP_%lu (RSSI=%d dBm)",
                     i + 1, (unsigned long)cache->sns[i], (int)cache->rssi[i]);
        }
    }
    free(aps);
}

static int build_prioritized_hosts(uint8_t *hosts, uint8_t own_host, uint8_t gw_host)
{
    int idx = 0;
    for (int h = 100; h <= 160; h++) {
        if (h != own_host && h != gw_host) hosts[idx++] = (uint8_t)h;
    }
    for (int h = 2; h <= 99; h++) {
        if (h != own_host && h != gw_host) hosts[idx++] = (uint8_t)h;
    }
    for (int h = 161; h <= 254; h++) {
        if (h != own_host && h != gw_host) hosts[idx++] = (uint8_t)h;
    }
    return idx;
}

static void tcp8899_scan_prefix(uint32_t base, uint32_t net_ip, uint8_t gw_host,
                                discovered_logger_t *list, int *count, int max_count,
                                bool blind_mode, bool stop_on_first,
                                ap_sn_cache_t *ap_cache, char tags[][24])
{
    const int BATCH = 6;
    int socks[6];
    uint32_t ips[6];

    uint8_t hosts[256];
    uint8_t own_host = (uint8_t)(net_ip & 0xFF);
    int total_hosts = build_prioritized_hosts(hosts, own_host, gw_host);

    int pos = 0;
    while (pos < total_hosts && *count < max_count) {
        if (stop_on_first && deye_modbus_is_scan_paused()) break;
        int n = 0;
        int max_fd = -1;
        fd_set wfds;
        FD_ZERO(&wfds);

        while (n < BATCH && pos < total_hosts) {
            uint32_t ip_h = base | hosts[pos++];
            int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s < 0) break;

            int flags = fcntl(s, F_GETFL, 0);
            fcntl(s, F_SETFL, flags | O_NONBLOCK);

            struct sockaddr_in dst = {0};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(SOLARMAN_TCP_PORT);
            dst.sin_addr.s_addr = htonl(ip_h);

            int rc = connect(s, (struct sockaddr *)&dst, sizeof(dst));
            if (rc < 0 && errno != EINPROGRESS) {
                close(s);
                continue;
            }
            socks[n] = s;
            ips[n] = ip_h;
            FD_SET(s, &wfds);
            if (s > max_fd) max_fd = s;
            n++;
        }

        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            break;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 650000 };
        int ready = select(max_fd + 1, NULL, &wfds, NULL, &tv);
        bool confirmed_found = false;

        for (int i = 0; i < n; i++) {
            bool open = false;
            if (ready > 0 && FD_ISSET(socks[i], &wfds)) {
                int so_err = 0;
                socklen_t slen = sizeof(so_err);
                if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) {
                    open = true;
                }
            }
            close(socks[i]);

            if (open) {
                struct in_addr ina = { .s_addr = htonl(ips[i]) };
                char cand_ip[16] = {0};
                ip4addr_ntoa_r((const ip4_addr_t *)&ina, cand_ip, sizeof(cand_ip));
                ESP_LOGI(TAG, "TCP 8899 open on %s -> resolving SN...", cand_ip);

                char mac[20] = {0};
                uint32_t sn = 0;
                const char *method = "TCP";

                if (udp_unicast_query(cand_ip, mac, sizeof(mac), &sn) && sn > 0) {
                    method = "TCP+UDP";
                } else if (http80_query_sn(cand_ip, &sn) && sn > 0) {
                    method = "TCP+HTTP";
                } else if (!blind_mode && g_settings.logger_sn > 0) {
                    sn = g_settings.logger_sn;
                    if (g_settings.logger_mac[0] != '\0') {
                        strncpy(mac, g_settings.logger_mac, sizeof(mac) - 1);
                    }
                    method = "TCP+CFG";
                } else {
                    ap_sn_ensure_scanned(ap_cache);
                    for (int a = 0; a < ap_cache->count; a++) {
                        uint16_t dummy = 0;
                        vTaskDelay(pdMS_TO_TICKS(120));
                        if (solarman_v5_read_registers(cand_ip, ap_cache->sns[a], 0, 1, &dummy) == ESP_OK ||
                            (vTaskDelay(pdMS_TO_TICKS(200)),
                             solarman_v5_read_registers(cand_ip, ap_cache->sns[a], 0, 1, &dummy) == ESP_OK)) {
                            sn = ap_cache->sns[a];
                            method = "TCP+AP_SN";
                            break;
                        }
                    }
                }

                add_discovered_unique(list, count, max_count, cand_ip, mac, sn, method, tags);
                if (sn > 0) confirmed_found = true;
            }
        }

        if (confirmed_found && stop_on_first) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void tcp8899_subnet_scan(const esp_netif_ip_info_t *sta_ip,
                                discovered_logger_t *list, int *count, int max_count,
                                bool blind_mode, bool stop_on_first, char tags[][24])
{
    if (!sta_ip || sta_ip->ip.addr == 0) return;

    uint32_t net_ip = ntohl(sta_ip->ip.addr);
    uint32_t net_mask = ntohl(sta_ip->netmask.addr);
    uint8_t gw_host = (uint8_t)(ntohl(sta_ip->gw.addr) & 0xFF);
    uint32_t base_primary = (net_ip & net_mask) & 0xFFFFFF00UL;
    uint32_t base_own24   = net_ip & 0xFFFFFF00UL;

    ap_sn_cache_t ap_cache = {0};
    ESP_LOGI(TAG, "Running prioritized TCP %d scan (650ms timeout, blind=%d)...",
             SOLARMAN_TCP_PORT, blind_mode ? 1 : 0);
    tcp8899_scan_prefix(base_primary, net_ip, gw_host, list, count, max_count,
                        blind_mode, stop_on_first, &ap_cache, tags);
    if (*count == 0 && base_own24 != base_primary) {
        tcp8899_scan_prefix(base_own24, net_ip, gw_host, list, count, max_count,
                            blind_mode, stop_on_first, &ap_cache, tags);
    }
}

int logger_discover_all_ex(discovered_logger_t *out_list, int max_count, bool blind_mode)
{
    if (!out_list || max_count <= 0) return 0;

    int64_t t0 = esp_timer_get_time();
    char tags[8][24] = {{0}};
    int cap = (max_count > 8) ? 8 : max_count;

    esp_netif_ip_info_t sta_ip = {0};
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif) {
        esp_netif_get_ip_info(sta_netif, &sta_ip);
    }

    int count = 0;

    if (!blind_mode && strlen(g_settings.logger_ip) >= 7) {
        char mac[20] = {0};
        uint32_t sn = 0;
        deye_data_t cur_d;
        deye_data_get(&cur_d);
        if (udp_unicast_query(g_settings.logger_ip, mac, sizeof(mac), &sn)) {
            add_discovered_unique(out_list, &count, cap, g_settings.logger_ip, mac, sn, "CFG+UDP", tags);
        } else if (cur_d.logger_connected && g_settings.logger_sn > 0) {
            add_discovered_unique(out_list, &count, cap,
                                  g_settings.logger_ip, g_settings.logger_mac, g_settings.logger_sn,
                                  "CFG", tags);
        }
    }

    if (count == 0) {
        udp_broadcast_discover(&sta_ip, out_list, &count, cap, tags);
    }

    if (count == 0) {
        tcp8899_subnet_scan(&sta_ip, out_list, &count, cap, blind_mode, blind_mode, tags);
    }

    int elapsed_ms = (int)((esp_timer_get_time() - t0) / 1000LL);
    deye_data_t cur_d;
    deye_data_get(&cur_d);
    for (int i = 0; i < count; i++) {
        uint16_t dev_type = 0;
        const char *mtag = tags[i][0] ? tags[i] : "DISC";
        if (out_list[i].sn > 0 &&
            solarman_v5_read_registers(out_list[i].ip, out_list[i].sn, 0, 1, &dev_type) == ESP_OK) {
            if (dev_type == 0x0500 || dev_type == 0x0600 || dev_type == 0x0601) {
                snprintf(out_list[i].model, sizeof(out_list[i].model),
                         "3F 0x%04X [%s %dms]", dev_type, mtag, elapsed_ms);
            } else {
                snprintf(out_list[i].model, sizeof(out_list[i].model),
                         "1F 0x%04X [%s %dms]", dev_type, mtag, elapsed_ms);
            }
        } else if (!blind_mode && cur_d.logger_connected && strcmp(out_list[i].ip, g_settings.logger_ip) == 0) {
            snprintf(out_list[i].model, sizeof(out_list[i].model),
                     "Deye [%s %dms]", mtag, elapsed_ms);
        } else {
            snprintf(out_list[i].model, sizeof(out_list[i].model),
                     "Solarman [%s %dms]", mtag, elapsed_ms);
        }
    }

    ESP_LOGI(TAG, "Discovery finished in %d ms: found=%d (blind=%d)", elapsed_ms, count, blind_mode ? 1 : 0);
    return count;
}

int logger_discover_all(discovered_logger_t *out_list, int max_count)
{
    return logger_discover_all_ex(out_list, max_count, false);
}

bool logger_discover_by_mac(const char *target_mac, uint32_t target_sn,
                            char *out_ip, size_t out_ip_sz,
                            char *out_mac, size_t out_mac_sz,
                            uint32_t *out_sn)
{
    char norm_target[20];
    normalize_mac(target_mac, norm_target, sizeof(norm_target));

    discovered_logger_t found[6];
    int n = logger_discover_all_ex(found, 6, (target_sn == 0 && norm_target[0] == '\0'));
    if (n <= 0) return false;

    // 1. Exact MAC or SN match
    for (int i = 0; i < n; i++) {
        bool mac_match = (norm_target[0] != '\0' && strcmp(norm_target, found[i].mac) == 0);
        bool sn_match = (target_sn > 0 && found[i].sn == target_sn);
        if (mac_match || sn_match) {
            strncpy(out_ip, found[i].ip, out_ip_sz - 1);
            out_ip[out_ip_sz - 1] = '\0';
            if (out_mac && out_mac_sz > 0 && found[i].mac[0] != '\0') {
                strncpy(out_mac, found[i].mac, out_mac_sz - 1);
                out_mac[out_mac_sz - 1] = '\0';
            }
            if (out_sn && found[i].sn > 0) *out_sn = found[i].sn;
            return true;
        }
    }

    // 2. Zero-config fallback: pick first responsive Solarman logger (or first with SN)
    for (int i = 0; i < n; i++) {
        if (found[i].sn > 0) {
            strncpy(out_ip, found[i].ip, out_ip_sz - 1);
            out_ip[out_ip_sz - 1] = '\0';
            if (out_mac && out_mac_sz > 0 && found[i].mac[0] != '\0') {
                strncpy(out_mac, found[i].mac, out_mac_sz - 1);
                out_mac[out_mac_sz - 1] = '\0';
            }
            if (out_sn) *out_sn = found[i].sn;
            ESP_LOGW(TAG, "Zero-config lock onto Solarman logger: IP=%s MAC=%s SN=%lu",
                     found[i].ip, found[i].mac, (unsigned long)found[i].sn);
            return true;
        }
    }

    // 3. Last resort: TCP 8899 open host + configured target_sn
    if (target_sn > 0) {
        for (int i = 0; i < n; i++) {
            uint16_t dev_type = 0;
            if (solarman_v5_read_registers(found[i].ip, target_sn, 0, 1, &dev_type) == ESP_OK) {
                strncpy(out_ip, found[i].ip, out_ip_sz - 1);
                out_ip[out_ip_sz - 1] = '\0';
                if (out_sn) *out_sn = target_sn;
                return true;
            }
        }
    }

    return false;
}
