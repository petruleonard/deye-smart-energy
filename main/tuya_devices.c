#include "tuya_devices.h"
#include "ac_control.h"
#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "mbedtls/md.h"
#include "psa/crypto.h"

static const char *TAG = "tuya_devices";
static const char *TUYA_EXTRA_PATH = "/spiffs/tuya_extra.json";

static SemaphoreHandle_t s_td_mutex = NULL;
static cJSON *s_store = NULL;
static cJSON *s_cache = NULL;

static void td_init_once(void) {
    if (!s_td_mutex) s_td_mutex = xSemaphoreCreateMutex();
    if (!s_cache) s_cache = cJSON_CreateObject();
    if (!s_store) {
        FILE *f = fopen(TUYA_EXTRA_PATH, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 32768) {
                char *buf = malloc((size_t)sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, (size_t)sz, f);
                    buf[n] = '\0';
                    s_store = cJSON_Parse(buf);
                    free(buf);
                }
            }
            fclose(f);
        }
        if (!s_store || !cJSON_IsObject(s_store)) {
            if (s_store) cJSON_Delete(s_store);
            s_store = cJSON_CreateObject();
        }
        if (!cJSON_IsArray(cJSON_GetObjectItem(s_store, "dehum_devices")))
            cJSON_AddItemToObject(s_store, "dehum_devices", cJSON_CreateArray());
        if (!cJSON_IsArray(cJSON_GetObjectItem(s_store, "sensor_devices")))
            cJSON_AddItemToObject(s_store, "sensor_devices", cJSON_CreateArray());
        if (!cJSON_IsArray(cJSON_GetObjectItem(s_store, "plug_devices")))
            cJSON_AddItemToObject(s_store, "plug_devices", cJSON_CreateArray());
    }
}

void tuya_devices_init(void) {
    td_init_once();
}

static void td_save_store(void) {
    if (!s_store) return;
    char *txt = cJSON_PrintUnformatted(s_store);
    if (!txt) return;
    FILE *f = fopen(TUYA_EXTRA_PATH, "w");
    if (f) {
        fwrite(txt, 1, strlen(txt), f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "Nu am putut scrie %s", TUYA_EXTRA_PATH);
    }
    free(txt);
}

static const char *kind_list_key(tuya_dev_kind_t k) {
    if (k == TUYA_KIND_DEHUM) return "dehum_devices";
    if (k == TUYA_KIND_SENSOR) return "sensor_devices";
    return "plug_devices";
}

static const char *kind_sel_key(tuya_dev_kind_t k) {
    if (k == TUYA_KIND_DEHUM) return "dehum_selected_device_id";
    if (k == TUYA_KIND_PLUG) return "plug_selected_device_id";
    return "";
}

static const char *kind_fallback_name(tuya_dev_kind_t k) {
    if (k == TUYA_KIND_DEHUM) return "Dezumidificator";
    if (k == TUYA_KIND_SENSOR) return "Senzor";
    return "Priza";
}

static const char *jstr(cJSON *obj, const char *key, const char *def) {
    cJSON *it = obj ? cJSON_GetObjectItem(obj, key) : NULL;
    return (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) ? it->valuestring : def;
}

static void td_any_creds(char *reg, size_t reg_sz, char *cid, size_t cid_sz, char *sec, size_t sec_sz) {
    reg[0] = cid[0] = sec[0] = '\0';
    if (g_settings.ac_tuya_region[0]) strncpy(reg, g_settings.ac_tuya_region, reg_sz - 1);
    if (g_settings.ac_tuya_client_id[0]) strncpy(cid, g_settings.ac_tuya_client_id, cid_sz - 1);
    if (g_settings.ac_tuya_client_secret[0]) strncpy(sec, g_settings.ac_tuya_client_secret, sec_sz - 1);
    const char *keys[3] = {"dehum_devices", "sensor_devices", "plug_devices"};
    for (int i = 0; i < 3 && (!cid[0] || !sec[0]); i++) {
        cJSON *arr = cJSON_GetObjectItem(s_store, keys[i]);
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, arr) {
            if (!reg[0] && jstr(d, "tuya_region", "")[0]) strncpy(reg, jstr(d, "tuya_region", ""), reg_sz - 1);
            if (!cid[0] && jstr(d, "tuya_client_id", "")[0]) strncpy(cid, jstr(d, "tuya_client_id", ""), cid_sz - 1);
            if (!sec[0] && jstr(d, "tuya_client_secret", "")[0]) strncpy(sec, jstr(d, "tuya_client_secret", ""), sec_sz - 1);
        }
    }
    if (!reg[0]) strncpy(reg, "eu", reg_sz - 1);
    reg[reg_sz - 1] = '\0';
    cid[cid_sz - 1] = '\0';
    sec[sec_sz - 1] = '\0';
}

void tuya_devices_get_fallback_creds(char *reg, size_t reg_sz, char *cid, size_t cid_sz, char *sec, size_t sec_sz) {
    td_init_once();
    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    td_any_creds(reg, reg_sz, cid, cid_sz, sec, sec_sz);
    xSemaphoreGive(s_td_mutex);
}

static void td_eff_creds(cJSON *dev, char *reg, size_t reg_sz, char *cid, size_t cid_sz, char *sec, size_t sec_sz) {
    td_any_creds(reg, reg_sz, cid, cid_sz, sec, sec_sz);
    if (jstr(dev, "tuya_region", "")[0]) { strncpy(reg, jstr(dev, "tuya_region", ""), reg_sz - 1); reg[reg_sz - 1] = '\0'; }
    if (jstr(dev, "tuya_client_id", "")[0]) { strncpy(cid, jstr(dev, "tuya_client_id", ""), cid_sz - 1); cid[cid_sz - 1] = '\0'; }
    if (jstr(dev, "tuya_client_secret", "")[0]) { strncpy(sec, jstr(dev, "tuya_client_secret", ""), sec_sz - 1); sec[sec_sz - 1] = '\0'; }
}

// ---------- Crypto helpers (AES-128-ECB, AES-128-GCM, HMAC-SHA256) ----------
static int psa_make_aes_key(const uint8_t key[16], psa_algorithm_t alg, psa_key_usage_t usage, psa_key_id_t *out_id) {
    psa_crypto_init();
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, alg);
    psa_set_key_usage_flags(&attr, usage);
    return (psa_import_key(&attr, key, 16, out_id) == PSA_SUCCESS) ? 0 : -1;
}

static int aes_ecb_crypt(const uint8_t key[16], const uint8_t *in, size_t in_len, uint8_t *out, bool enc, size_t *out_len) {
    psa_key_id_t kid = 0;
    if (psa_make_aes_key(key, PSA_ALG_ECB_NO_PADDING, enc ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT, &kid) != 0) return -1;
    psa_status_t st = PSA_ERROR_GENERIC_ERROR;
    if (enc) {
        uint8_t pad = (uint8_t)(16 - (in_len % 16));
        size_t tot = in_len + pad;
        uint8_t *tmp = malloc(tot);
        if (tmp) {
            memcpy(tmp, in, in_len);
            memset(tmp + in_len, pad, pad);
            size_t olen = 0;
            st = psa_cipher_encrypt(kid, PSA_ALG_ECB_NO_PADDING, tmp, tot, out, tot + 16, &olen);
            free(tmp);
            *out_len = olen;
        }
    } else {
        if (in_len > 0 && (in_len % 16) == 0) {
            size_t olen = 0;
            st = psa_cipher_decrypt(kid, PSA_ALG_ECB_NO_PADDING, in, in_len, out, in_len, &olen);
            if (st == PSA_SUCCESS && olen > 0) {
                uint8_t pad = out[olen - 1];
                *out_len = (pad >= 1 && pad <= 16 && pad <= olen) ? (olen - pad) : olen;
            }
        }
    }
    psa_destroy_key(kid);
    return (st == PSA_SUCCESS) ? 0 : -1;
}

static int aes_gcm_crypt(const uint8_t key[16], const uint8_t iv[12], const uint8_t *aad, size_t aad_len,
                         const uint8_t *in, size_t len, uint8_t *out, uint8_t tag[16], bool enc) {
    psa_key_id_t kid = 0;
    if (psa_make_aes_key(key, PSA_ALG_GCM, enc ? PSA_KEY_USAGE_ENCRYPT : PSA_KEY_USAGE_DECRYPT, &kid) != 0) return -1;
    uint8_t *tmp = malloc(len + 16);
    if (!tmp) { psa_destroy_key(kid); return -1; }
    psa_status_t st;
    size_t olen = 0;
    if (enc) {
        st = psa_aead_encrypt(kid, PSA_ALG_GCM, iv, 12, aad, aad_len, in, len, tmp, len + 16, &olen);
        if (st == PSA_SUCCESS && olen >= 16) {
            memcpy(out, tmp, olen - 16);
            memcpy(tag, tmp + olen - 16, 16);
        }
    } else {
        memcpy(tmp, in, len);
        memcpy(tmp + len, tag, 16);
        st = psa_aead_decrypt(kid, PSA_ALG_GCM, iv, 12, aad, aad_len, tmp, len + 16, out, len + 16, &olen);
    }
    free(tmp);
    psa_destroy_key(kid);
    return (st == PSA_SUCCESS) ? 0 : -1;
}

static void hmac_sha256_raw(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    psa_crypto_init();
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr, key_len * 8);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_key_id_t kid = 0;
    if (psa_import_key(&attr, key, key_len, &kid) == PSA_SUCCESS) {
        size_t mac_len = 0;
        psa_mac_compute(kid, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, 32, &mac_len);
        psa_destroy_key(kid);
    }
}

static void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}

static uint32_t rd_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---------- UDP LAN Discovery (6667 / 6666 / 7000) ----------
typedef struct {
    char id[40];
    char ip[20];
    char ver[8];
} lan_disc_entry_t;

static void tuya_lan_udp_scan(lan_disc_entry_t *entries, int max_entries, int *out_count, int timeout_ms) {
    *out_count = 0;
    uint8_t udp_key[16];
    const char *seed = "yGAdlopoPVldABfn";
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_MD5), (const uint8_t *)seed, strlen(seed), udp_key);

    const int ports[3] = {6667, 6666, 7000};
    int socks[3] = {-1, -1, -1};
    for (int i = 0; i < 3; i++) {
        int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) continue;
        int opt = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(ports[i]);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            struct timeval tv = {.tv_sec = 0, .tv_usec = 200000};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            socks[i] = s;
        } else {
            close(s);
        }
    }

    uint8_t *buf = malloc(1536);
    uint8_t *dec = malloc(1536);
    if (buf && dec) {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while (xTaskGetTickCount() < deadline && *out_count < max_entries) {
            for (int i = 0; i < 3; i++) {
                if (socks[i] < 0) continue;
                struct sockaddr_in from = {0};
                socklen_t flen = sizeof(from);
                int r = recvfrom(socks[i], buf, 1500, 0, (struct sockaddr *)&from, &flen);
                if (r <= 28) continue;
                uint32_t pref = rd_be32(buf);
                size_t dlen = 0;
                bool ok = false;
                if (pref == 0x000055AA && r > 28) {
                    if (ports[i] == 6666) {
                        dlen = (size_t)(r - 28);
                        memcpy(dec, buf + 20, dlen);
                        ok = true;
                    } else {
                        ok = (aes_ecb_crypt(udp_key, buf + 20, (size_t)(r - 28), dec, false, &dlen) == 0);
                    }
                } else if (pref == 0x00006699 && r > 38) {
                    size_t ct_len = (size_t)(r - 50);
                    if (aes_gcm_crypt(udp_key, buf + 18, buf + 4, 14, buf + 30, ct_len, dec, buf + r - 20, false) == 0) {
                        dlen = ct_len;
                        if (dlen >= 4 && rd_be32(dec) == 0) { memmove(dec, dec + 4, dlen - 4); dlen -= 4; }
                        ok = true;
                    }
                }
                if (!ok || dlen == 0 || dlen >= 1500) continue;
                dec[dlen] = '\0';
                cJSON *j = cJSON_Parse((char *)dec);
                if (!j) continue;
                const char *gid = jstr(j, "gwId", jstr(j, "devId", ""));
                if (gid[0]) {
                    bool dup = false;
                    for (int k = 0; k < *out_count; k++) {
                        if (strcmp(entries[k].id, gid) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        lan_disc_entry_t *e = &entries[*out_count];
                        strncpy(e->id, gid, sizeof(e->id) - 1);
                        const char *jip = jstr(j, "ip", "");
                        if (jip[0]) strncpy(e->ip, jip, sizeof(e->ip) - 1);
                        else inet_ntoa_r(from.sin_addr, e->ip, sizeof(e->ip));
                        strncpy(e->ver, jstr(j, "version", "3.3"), sizeof(e->ver) - 1);
                        (*out_count)++;
                    }
                }
                cJSON_Delete(j);
            }
        }
    }
    free(buf);
    free(dec);
    for (int i = 0; i < 3; i++) if (socks[i] >= 0) close(socks[i]);
}

// ---------- TCP Tuya LAN Client (v3.3 / v3.4 / v3.5) ----------
static int tcp_connect_tmo(const char *ip, int port, int tmo_ms) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    struct timeval tv = {.tv_sec = tmo_ms / 1000, .tv_usec = (tmo_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_aton(ip, &addr.sin_addr) == 0) { close(s); return -1; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(s); return -1; }
    return s;
}

static bool tcp_recv_pkt_55aa(int s, uint8_t *buf, size_t buf_sz, size_t *body_off, size_t *body_len, bool is_v34) {
    size_t got = 0;
    while (got < 16) {
        int r = recv(s, buf + got, 16 - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    if (rd_be32(buf) != 0x000055AA) return false;
    uint32_t plen = rd_be32(buf + 12);
    size_t need = 16 + plen;
    if (need > buf_sz || plen < (is_v34 ? 36U : 8U)) return false;
    while (got < need) {
        int r = recv(s, buf + got, need - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < need) return false;
    size_t off = 16;
    size_t blen = need - 16 - (is_v34 ? 36 : 8);
    if (blen >= 4 && rd_be32(buf + off) == 0) { off += 4; blen -= 4; }
    *body_off = off;
    *body_len = blen;
    return true;
}

static bool tuya_lan_v33(const char *did, const char *ip, const char *lkey, const char *dps_json, cJSON **out_dps, char *err, size_t err_sz) {
    uint8_t key[16] = {0};
    memcpy(key, lkey, strlen(lkey) > 16 ? 16 : strlen(lkey));
    char *plain = malloc(512);
    uint8_t *pkt = malloc(768);
    uint8_t *rx = malloc(1536);
    if (!plain || !pkt || !rx) { free(plain); free(pkt); free(rx); return false; }

    long long ts = (long long)time(NULL);
    if (!dps_json) snprintf(plain, 512, "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\"}", did, did, did, ts);
    else snprintf(plain, 512, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}", did, did, ts, dps_json);

    size_t hdr_p = 16 + (dps_json ? 15 : 0);
    size_t enc_len = 0;
    if (dps_json) { memcpy(pkt + 16, "3.3\0\0\0\0\0\0\0\0\0\0\0\0", 15); }
    aes_ecb_crypt(key, (uint8_t *)plain, strlen(plain), pkt + hdr_p, true, &enc_len);
    size_t pay_len = (dps_json ? 15 : 0) + enc_len;
    wr_be32(pkt, 0x000055AA); wr_be32(pkt + 4, 1); wr_be32(pkt + 8, dps_json ? 0x07 : 0x0A); wr_be32(pkt + 12, (uint32_t)(pay_len + 8));
    uint32_t crc = esp_rom_crc32_le(0, pkt, 16 + pay_len);
    wr_be32(pkt + 16 + pay_len, crc); wr_be32(pkt + 20 + pay_len, 0x0000AA55);

    int s = tcp_connect_tmo(ip, 6668, 3000);
    bool ok = false;
    if (s >= 0) {
        if (send(s, pkt, 24 + pay_len, 0) > 0) {
            size_t boff = 0, blen = 0;
            if (tcp_recv_pkt_55aa(s, rx, 1536, &boff, &blen, false)) {
                if (blen >= 15 && memcmp(rx + boff, "3.3", 3) == 0) { boff += 15; blen -= 15; }
                if (blen == 0) { *out_dps = cJSON_CreateObject(); ok = true; }
                else {
                    size_t dlen = 0;
                    if (aes_ecb_crypt(key, rx + boff, blen, (uint8_t *)plain, false, &dlen) == 0 && dlen < 512) {
                        plain[dlen] = '\0';
                        cJSON *j = cJSON_Parse(plain);
                        cJSON *dp = j ? cJSON_GetObjectItem(j, "dps") : NULL;
                        *out_dps = dp ? cJSON_Duplicate(dp, true) : cJSON_CreateObject();
                        if (j) cJSON_Delete(j);
                        ok = true;
                    }
                }
            } else snprintf(err, err_sz, "v3.3 timeout");
        }
        close(s);
    } else snprintf(err, err_sz, "TCP %s:6668 refuzat", ip);
    free(plain); free(pkt); free(rx);
    return ok;
}

static bool tuya_lan_v34(const char *did, const char *ip, const char *lkey, const char *dps_json, cJSON **out_dps, char *err, size_t err_sz) {
    uint8_t key[16] = {0}, cli_nonce[16], srv_nonce[16], hmac_b[32], xored[16], sess_full[32], sess_key[16];
    memcpy(key, lkey, strlen(lkey) > 16 ? 16 : strlen(lkey));
    esp_fill_random(cli_nonce, 16);

    uint8_t *pkt = malloc(768);
    uint8_t *rx = malloc(1536);
    char *plain = malloc(512);
    if (!pkt || !rx || !plain) { free(pkt); free(rx); free(plain); return false; }

    int s = tcp_connect_tmo(ip, 6668, 3000);
    bool ok = false;
    if (s >= 0) {
        size_t enc_len = 0, boff = 0, blen = 0, dlen = 0;
        wr_be32(pkt, 0x000055AA); wr_be32(pkt + 4, 1); wr_be32(pkt + 8, 0x03); wr_be32(pkt + 12, 16 + 36);
        memcpy(pkt + 16, cli_nonce, 16);
        hmac_sha256_raw(key, 16, pkt, 32, pkt + 32);
        wr_be32(pkt + 64, 0x0000AA55);
        if (send(s, pkt, 68, 0) > 0 && tcp_recv_pkt_55aa(s, rx, 1536, &boff, &blen, true) &&
            aes_ecb_crypt(key, rx + boff, blen, (uint8_t *)plain, false, &dlen) == 0 && dlen >= 48) {
            memcpy(srv_nonce, plain, 16);
            hmac_sha256_raw(key, 16, cli_nonce, 16, hmac_b);
            if (memcmp(plain + 16, hmac_b, 32) == 0) {
                hmac_sha256_raw(key, 16, srv_nonce, 16, hmac_b);
                wr_be32(pkt, 0x000055AA); wr_be32(pkt + 4, 2); wr_be32(pkt + 8, 0x05); wr_be32(pkt + 12, 32 + 36);
                memcpy(pkt + 16, hmac_b, 32);
                hmac_sha256_raw(key, 16, pkt, 48, pkt + 48);
                wr_be32(pkt + 80, 0x0000AA55);
                send(s, pkt, 84, 0);
                for (int i = 0; i < 16; i++) xored[i] = cli_nonce[i] ^ srv_nonce[i];
                size_t sl = 0;
                aes_ecb_crypt(key, xored, 16, sess_full, true, &sl);
                memcpy(sess_key, sess_full, 16);

                long long ts = (long long)time(NULL);
                if (!dps_json) snprintf(plain, 512, "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\"}", did, did, did, ts);
                else snprintf(plain, 512, "3.4\0\0\0\0\0\0\0\0\0\0\0\0{\"protocol\":5,\"t\":%lld,\"data\":{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}}", ts, did, did, ts, dps_json);
                size_t raw_len = dps_json ? (15 + strlen(plain + 15)) : strlen(plain);
                aes_ecb_crypt(sess_key, (uint8_t *)plain, raw_len, pkt + 16, true, &enc_len);
                wr_be32(pkt, 0x000055AA); wr_be32(pkt + 4, 3); wr_be32(pkt + 8, dps_json ? 0x0D : 0x10); wr_be32(pkt + 12, (uint32_t)(enc_len + 36));
                hmac_sha256_raw(sess_key, 16, pkt, 16 + enc_len, pkt + 16 + enc_len);
                wr_be32(pkt + 16 + enc_len + 32, 0x0000AA55);
                if (send(s, pkt, 16 + enc_len + 36, 0) > 0 && tcp_recv_pkt_55aa(s, rx, 1536, &boff, &blen, true)) {
                    if (blen == 0) { *out_dps = cJSON_CreateObject(); ok = true; }
                    else if (aes_ecb_crypt(sess_key, rx + boff, blen, (uint8_t *)plain, false, &dlen) == 0 && dlen < 512) {
                        size_t off = (dlen >= 15 && memcmp(plain, "3.4", 3) == 0) ? 15 : 0;
                        plain[dlen] = '\0';
                        cJSON *j = cJSON_Parse(plain + off);
                        cJSON *data_o = j ? cJSON_GetObjectItem(j, "data") : NULL;
                        cJSON *dp = data_o ? cJSON_GetObjectItem(data_o, "dps") : (j ? cJSON_GetObjectItem(j, "dps") : NULL);
                        *out_dps = dp ? cJSON_Duplicate(dp, true) : cJSON_CreateObject();
                        if (j) cJSON_Delete(j);
                        ok = true;
                    }
                } else snprintf(err, err_sz, "v3.4 timeout");
            } else snprintf(err, err_sz, "v3.4 key invalid");
        } else snprintf(err, err_sz, "v3.4 handshake esuat");
        close(s);
    } else snprintf(err, err_sz, "TCP %s:6668 refuzat", ip);
    free(pkt); free(rx); free(plain);
    return ok;
}

static bool recv_pkt_6699(int s, const uint8_t key[16], uint8_t *rx, size_t rx_sz, uint8_t *dec, size_t *dec_len) {
    size_t got = 0;
    while (got < 18) {
        int r = recv(s, rx + got, 18 - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    if (rd_be32(rx) != 0x00006699) return false;
    uint32_t plen = rd_be32(rx + 14);
    size_t need = 18 + plen + 4;
    if (need > rx_sz || plen < 28) return false;
    while (got < need) {
        int r = recv(s, rx + got, need - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got < need) return false;
    size_t ct_len = plen - 28;
    if (aes_gcm_crypt(key, rx + 18, rx + 4, 14, rx + 30, ct_len, dec, rx + 18 + plen - 16, false) != 0) return false;
    if (ct_len >= 4 && rd_be32(dec) == 0) { memmove(dec, dec + 4, ct_len - 4); ct_len -= 4; }
    *dec_len = ct_len;
    return true;
}

static size_t pack_6699(uint32_t seq, uint32_t cmd, const uint8_t *pay, size_t plen, const uint8_t key[16], uint8_t *out) {
    uint8_t iv[12], tag[16];
    esp_fill_random(iv, 12);
    uint32_t tot = (uint32_t)(12 + plen + 16);
    wr_be32(out, 0x00006699);
    out[4] = 0; out[5] = 0;
    wr_be32(out + 6, seq);
    wr_be32(out + 10, cmd);
    wr_be32(out + 14, tot);
    memcpy(out + 18, iv, 12);
    aes_gcm_crypt(key, iv, out + 4, 14, pay, plen, out + 30, tag, true);
    memcpy(out + 30 + plen, tag, 16);
    wr_be32(out + 30 + plen + 16, 0x00009966);
    return 30 + plen + 20;
}

static bool tuya_lan_v35(const char *did, const char *ip, const char *lkey, const char *dps_json, cJSON **out_dps, char *err, size_t err_sz) {
    uint8_t key[16] = {0}, cli_nonce[16], srv_nonce[16], hmac_b[32], xored[16], sess_key[16], tag[16];
    memcpy(key, lkey, strlen(lkey) > 16 ? 16 : strlen(lkey));
    esp_fill_random(cli_nonce, 16);

    uint8_t *pkt = malloc(768);
    uint8_t *rx = malloc(1536);
    char *plain = malloc(512);
    if (!pkt || !rx || !plain) { free(pkt); free(rx); free(plain); return false; }

    int s = tcp_connect_tmo(ip, 6668, 3000);
    bool ok = false;
    if (s >= 0) {
        size_t dlen = 0;
        size_t n1 = pack_6699(1, 0x03, cli_nonce, 16, key, pkt);
        if (send(s, pkt, n1, 0) > 0 && recv_pkt_6699(s, key, rx, 1536, (uint8_t *)plain, &dlen) && dlen >= 48) {
            memcpy(srv_nonce, plain, 16);
            hmac_sha256_raw(key, 16, cli_nonce, 16, hmac_b);
            if (memcmp(plain + 16, hmac_b, 32) == 0) {
                hmac_sha256_raw(key, 16, srv_nonce, 16, hmac_b);
                size_t n2 = pack_6699(2, 0x05, hmac_b, 32, key, pkt);
                send(s, pkt, n2, 0);
                for (int i = 0; i < 16; i++) xored[i] = cli_nonce[i] ^ srv_nonce[i];
                aes_gcm_crypt(key, cli_nonce, NULL, 0, xored, 16, sess_key, tag, true);

                long long ts = (long long)time(NULL);
                if (!dps_json) snprintf(plain, 512, "{\"gwId\":\"%s\",\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\"}", did, did, did, ts);
                else snprintf(plain, 512, "3.5\0\0\0\0\0\0\0\0\0\0\0\0{\"protocol\":5,\"t\":%lld,\"data\":{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lld\",\"dps\":%s}}", ts, did, did, ts, dps_json);
                size_t raw_len = dps_json ? (15 + strlen(plain + 15)) : strlen(plain);
                size_t n3 = pack_6699(3, dps_json ? 0x0D : 0x10, (uint8_t *)plain, raw_len, sess_key, pkt);
                if (send(s, pkt, n3, 0) > 0 && recv_pkt_6699(s, sess_key, rx, 1536, (uint8_t *)plain, &dlen)) {
                    if (dlen == 0) { *out_dps = cJSON_CreateObject(); ok = true; }
                    else if (dlen < 512) {
                        size_t off = (dlen >= 15 && memcmp(plain, "3.5", 3) == 0) ? 15 : 0;
                        plain[dlen] = '\0';
                        cJSON *j = cJSON_Parse(plain + off);
                        cJSON *data_o = j ? cJSON_GetObjectItem(j, "data") : NULL;
                        cJSON *dp = data_o ? cJSON_GetObjectItem(data_o, "dps") : (j ? cJSON_GetObjectItem(j, "dps") : NULL);
                        *out_dps = dp ? cJSON_Duplicate(dp, true) : cJSON_CreateObject();
                        if (j) cJSON_Delete(j);
                        ok = true;
                    }
                } else snprintf(err, err_sz, "v3.5 timeout");
            } else snprintf(err, err_sz, "v3.5 key invalid");
        } else snprintf(err, err_sz, "v3.5 handshake esuat");
        close(s);
    } else snprintf(err, err_sz, "TCP %s:6668 refuzat", ip);
    free(pkt); free(rx); free(plain);
    return ok;
}

static bool tuya_lan_req(const char *did, const char *ip_in, const char *lkey, const char *ver_in,
                         const char *dps_json, cJSON **out_dps, char *err, size_t err_sz) {
    if (!did[0] || !lkey[0]) { snprintf(err, err_sz, "Lipsesc Device ID sau Local Key"); return false; }
    char ip[24] = {0}, ver[8] = {0};
    strncpy(ip, ip_in, sizeof(ip) - 1);
    strncpy(ver, ver_in[0] ? ver_in : "auto", sizeof(ver) - 1);
    if (!ip[0]) {
        lan_disc_entry_t entries[6];
        int cnt = 0;
        tuya_lan_udp_scan(entries, 6, &cnt, 2500);
        for (int i = 0; i < cnt; i++) {
            if (strcmp(entries[i].id, did) == 0) {
                strncpy(ip, entries[i].ip, sizeof(ip) - 1);
                if (strcmp(ver, "auto") == 0) strncpy(ver, entries[i].ver, sizeof(ver) - 1);
                break;
            }
        }
    }
    if (!ip[0]) { snprintf(err, err_sz, "Lipseste IP local (negasit pe LAN)"); return false; }

    if (strcmp(ver, "3.5") == 0) return tuya_lan_v35(did, ip, lkey, dps_json, out_dps, err, err_sz);
    if (strcmp(ver, "3.4") == 0) return tuya_lan_v34(did, ip, lkey, dps_json, out_dps, err, err_sz);
    if (strcmp(ver, "3.3") == 0) return tuya_lan_v33(did, ip, lkey, dps_json, out_dps, err, err_sz);

    if (tuya_lan_v33(did, ip, lkey, dps_json, out_dps, err, err_sz)) return true;
    if (tuya_lan_v34(did, ip, lkey, dps_json, out_dps, err, err_sz)) return true;
    return tuya_lan_v35(did, ip, lkey, dps_json, out_dps, err, err_sz);
}

// ---------- Cloud read & write ----------
static bool tuya_cloud_read_dps(const char *reg, const char *cid, const char *sec, const char *did, cJSON **out_dps, char *err, size_t err_sz) {
    if (!ac_tuya_get_token(reg, cid, sec, false, err, err_sz)) return false;
    char path[128];
    snprintf(path, sizeof(path), "/v2.0/cloud/thing/%s/shadow/properties", did);
    char *resp = malloc(4096);
    if (!resp) return false;
    int code = ac_tuya_signed_req(reg, cid, sec, HTTP_METHOD_GET, path, NULL, resp, 4096);
    cJSON *j = cJSON_Parse(resp);
    if (code == 200 && j && !cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
        cJSON *cj = cJSON_GetObjectItem(j, "code");
        if (cJSON_IsNumber(cj) && cj->valueint == 1010) {
            cJSON_Delete(j);
            if (ac_tuya_get_token(reg, cid, sec, true, err, err_sz)) {
                code = ac_tuya_signed_req(reg, cid, sec, HTTP_METHOD_GET, path, NULL, resp, 4096);
                j = cJSON_Parse(resp);
            } else j = NULL;
        }
    }
    free(resp);
    if (code == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
        cJSON *res = cJSON_GetObjectItem(j, "result");
        cJSON *props = res ? cJSON_GetObjectItem(res, "properties") : NULL;
        cJSON *map = cJSON_CreateObject();
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, props) {
            cJSON *ck = cJSON_GetObjectItem(p, "code");
            cJSON *cv = cJSON_GetObjectItem(p, "value");
            if (cJSON_IsString(ck) && cv) cJSON_AddItemToObject(map, ck->valuestring, cJSON_Duplicate(cv, true));
        }
        cJSON_Delete(j);
        *out_dps = map;
        return true;
    }
    cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
    snprintf(err, err_sz, "%s", cJSON_IsString(m) ? m->valuestring : "Eroare citire Tuya Cloud");
    if (j) cJSON_Delete(j);
    return false;
}

static bool tuya_cloud_write_props(const char *reg, const char *cid, const char *sec, const char *did, cJSON *props, char *err, size_t err_sz) {
    if (!ac_tuya_get_token(reg, cid, sec, false, err, err_sz)) return false;
    cJSON *wrap = cJSON_CreateObject();
    cJSON_AddItemToObject(wrap, "properties", cJSON_Duplicate(props, true));
    char *body = cJSON_PrintUnformatted(wrap);
    cJSON_Delete(wrap);
    char path[128];
    snprintf(path, sizeof(path), "/v2.0/cloud/thing/%s/shadow/properties/issue", did);
    char *resp = malloc(2048);
    if (!resp || !body) { free(resp); free(body); return false; }
    int code = ac_tuya_signed_req(reg, cid, sec, HTTP_METHOD_POST, path, body, resp, 2048);
    free(body);
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    bool ok = (code == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "success")));
    if (!ok) {
        cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
        snprintf(err, err_sz, "%s", cJSON_IsString(m) ? m->valuestring : "Eroare comanda Tuya Cloud");
    }
    if (j) cJSON_Delete(j);
    return ok;
}

static bool dev_read_dps(cJSON *dev, cJSON **out_dps, char *used_out, size_t used_sz, char *err, size_t err_sz) {
    const char *did = jstr(dev, "id", "");
    const char *mode = jstr(dev, "conn_mode", "auto");
    const char *ip = jstr(dev, "ip", "");
    const char *lkey = jstr(dev, "local_key", "");
    const char *ver = jstr(dev, "version", "auto");
    char reg[8], cid[48], sec[64];
    td_eff_creds(dev, reg, sizeof(reg), cid, sizeof(cid), sec, sizeof(sec));

    char lan_err[96] = {0};
    if ((strcmp(mode, "lan") == 0 || strcmp(mode, "auto") == 0) && ip[0] && lkey[0]) {
        if (tuya_lan_req(did, ip, lkey, ver, NULL, out_dps, lan_err, sizeof(lan_err))) {
            strncpy(used_out, "lan", used_sz - 1);
            return true;
        }
        if (strcmp(mode, "lan") == 0) {
            strncpy(err, lan_err, err_sz - 1);
            strncpy(used_out, "lan", used_sz - 1);
            return false;
        }
    }
    if ((strcmp(mode, "cloud") == 0 || strcmp(mode, "auto") == 0) && cid[0] && sec[0] && did[0]) {
        if (tuya_cloud_read_dps(reg, cid, sec, did, out_dps, err, err_sz)) {
            strncpy(used_out, "cloud", used_sz - 1);
            return true;
        }
        strncpy(used_out, "cloud", used_sz - 1);
        return false;
    }
    snprintf(err, err_sz, "%s", lan_err[0] ? lan_err : "Dispozitiv neconfigurat");
    strncpy(used_out, mode, used_sz - 1);
    return false;
}

// ---------- State mappers ----------
static cJSON *dp_find(cJSON *dps, const char *k1, const char *k2) {
    cJSON *v = dps ? cJSON_GetObjectItem(dps, k1) : NULL;
    if (!v && k2 && dps) v = cJSON_GetObjectItem(dps, k2);
    return v;
}

static double plug_update_daily_kwh(const char *did, double watts, double *out_avg_w) {
    if (out_avg_w) *out_avg_w = watts;
    if (!did || !did[0]) return 0.0;
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char today[16];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);

    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    cJSON *stats = cJSON_GetObjectItem(s_store, "plug_energy_stats");
    if (!cJSON_IsObject(stats)) {
        cJSON_DeleteItemFromObject(s_store, "plug_energy_stats");
        stats = cJSON_AddObjectToObject(s_store, "plug_energy_stats");
    }
    cJSON *rec = cJSON_GetObjectItem(stats, did);
    double kwh = 0.0;
    double start_ts = (double)now;
    if (!cJSON_IsObject(rec) || strcmp(jstr(rec, "date", ""), today) != 0) {
        cJSON_DeleteItemFromObject(stats, did);
        rec = cJSON_AddObjectToObject(stats, did);
        cJSON_AddStringToObject(rec, "date", today);
        cJSON_AddNumberToObject(rec, "kwh", 0.0);
        cJSON_AddNumberToObject(rec, "start_ts", (double)now);
        cJSON_AddNumberToObject(rec, "last_ts", (double)now);
        cJSON_AddNumberToObject(rec, "last_w", watts);
    } else {
        cJSON *kj = cJSON_GetObjectItem(rec, "kwh");
        cJSON *sj = cJSON_GetObjectItem(rec, "start_ts");
        cJSON *tj = cJSON_GetObjectItem(rec, "last_ts");
        cJSON *wj = cJSON_GetObjectItem(rec, "last_w");
        kwh = cJSON_IsNumber(kj) ? kj->valuedouble : 0.0;
        double last_ts = cJSON_IsNumber(tj) ? tj->valuedouble : (double)now;
        start_ts = cJSON_IsNumber(sj) ? sj->valuedouble : last_ts;
        double last_w = cJSON_IsNumber(wj) ? wj->valuedouble : watts;
        double dt = (double)now - last_ts;
        if (dt > 0.0 && dt <= 14400.0) {
            kwh += (((last_w + watts) / 2.0) * dt) / 3600000.0;
        }
        cJSON_DeleteItemFromObject(rec, "kwh"); cJSON_AddNumberToObject(rec, "kwh", kwh);
        cJSON_DeleteItemFromObject(rec, "start_ts"); cJSON_AddNumberToObject(rec, "start_ts", start_ts);
        cJSON_DeleteItemFromObject(rec, "last_ts"); cJSON_AddNumberToObject(rec, "last_ts", (double)now);
        cJSON_DeleteItemFromObject(rec, "last_w"); cJSON_AddNumberToObject(rec, "last_w", watts);
    }
    td_save_store();
    xSemaphoreGive(s_td_mutex);
    double elapsed_h = ((double)now - start_ts) / 3600.0;
    if (out_avg_w) *out_avg_w = (elapsed_h >= (10.0 / 3600.0)) ? (kwh * 1000.0 / elapsed_h) : watts;
    return kwh;
}

static cJSON *dps_to_state(tuya_dev_kind_t kind, cJSON *dps, const char *did) {
    cJSON *st = cJSON_CreateObject();
    if (kind == TUYA_KIND_DEHUM) {
        cJSON *pw = dp_find(dps, "switch", "1");
        cJSON *ht = dp_find(dps, "dehumidify_set_enum", "3");
        cJSON *hc = dp_find(dps, "humidity_indoor", "6");
        cJSON *md = dp_find(dps, "mode", "5");
        cJSON *fs = dp_find(dps, "fan_speed_enum", "4");
        cJSON *an = dp_find(dps, "anion", "10");
        cJSON *lk = dp_find(dps, "child_lock", "16");
        int hti = 50;
        if (cJSON_IsString(ht)) hti = atoi(ht->valuestring);
        else if (cJSON_IsNumber(ht)) hti = ht->valueint;
        if (hti < 30 || hti > 80) hti = 50;
        cJSON_AddBoolToObject(st, "power", cJSON_IsTrue(pw));
        cJSON_AddNumberToObject(st, "target_humidity", hti);
        if (cJSON_IsNumber(hc)) cJSON_AddNumberToObject(st, "current_humidity", hc->valueint);
        else cJSON_AddNullToObject(st, "current_humidity");
        cJSON_AddStringToObject(st, "mode", cJSON_IsString(md) ? md->valuestring : "dehumidify");
        cJSON_AddStringToObject(st, "fan_speed", cJSON_IsString(fs) ? fs->valuestring : "1");
        cJSON_AddBoolToObject(st, "anion", cJSON_IsTrue(an));
        cJSON_AddBoolToObject(st, "child_lock", cJSON_IsTrue(lk));
    } else if (kind == TUYA_KIND_SENSOR) {
        cJSON *tp = dp_find(dps, "temp_current", "1");
        cJSON *hm = dp_find(dps, "humidity_value", "2");
        cJSON *bt = dp_find(dps, "battery_percentage", "4");
        if (cJSON_IsNumber(tp)) {
            double tv = tp->valuedouble;
            cJSON_AddNumberToObject(st, "temp", (tv > 80.0 || tv < -80.0) ? (tv / 10.0) : tv);
        } else cJSON_AddNullToObject(st, "temp");
        if (cJSON_IsNumber(hm)) cJSON_AddNumberToObject(st, "humidity", hm->valueint);
        else cJSON_AddNullToObject(st, "humidity");
        if (cJSON_IsNumber(bt)) cJSON_AddNumberToObject(st, "battery", bt->valueint);
        else cJSON_AddNullToObject(st, "battery");
    } else {
        cJSON *pw = dp_find(dps, "switch_1", "1");
        if (!pw) pw = cJSON_GetObjectItem(dps, "switch");
        cJSON *wp = dp_find(dps, "cur_power", "19");
        cJSON *vv = dp_find(dps, "cur_voltage", "20");
        cJSON *cc = dp_find(dps, "cur_current", "18");
        cJSON_AddBoolToObject(st, "power", cJSON_IsTrue(pw));
        double watts = cJSON_IsNumber(wp) ? (wp->valuedouble / 10.0) : 0.0;
        double avg_w = watts;
        double kwh = plug_update_daily_kwh(did, watts, &avg_w);
        cJSON_AddNumberToObject(st, "watts", watts);
        cJSON_AddNumberToObject(st, "avg_watts", avg_w);
        cJSON_AddNumberToObject(st, "voltage", cJSON_IsNumber(vv) ? (vv->valuedouble / 10.0) : 0.0);
        cJSON_AddNumberToObject(st, "current_ma", cJSON_IsNumber(cc) ? cc->valueint : 0);
        cJSON_AddNumberToObject(st, "kwh_today", kwh);
    }
    return st;
}

static cJSON *safe_dev_view(cJSON *d, const char *def_reg, const char *def_cid, const char *def_sec, const char *fb_name) {
    cJSON *v = cJSON_CreateObject();
    const char *did = jstr(d, "id", "");
    cJSON_AddStringToObject(v, "id", did);
    cJSON_AddStringToObject(v, "name", jstr(d, "name", fb_name));
    cJSON_AddStringToObject(v, "conn_mode", jstr(d, "conn_mode", "auto"));
    cJSON_AddStringToObject(v, "ip", jstr(d, "ip", ""));
    const char *lk = jstr(d, "local_key", "");
    cJSON_AddStringToObject(v, "local_key", lk);
    cJSON_AddBoolToObject(v, "local_key_set", lk[0] != '\0');
    cJSON_AddStringToObject(v, "version", jstr(d, "version", "auto"));
    cJSON_AddStringToObject(v, "tuya_region", jstr(d, "tuya_region", def_reg));
    cJSON_AddStringToObject(v, "tuya_client_id", jstr(d, "tuya_client_id", def_cid));
    const char *sec = jstr(d, "tuya_client_secret", def_sec);
    cJSON_AddStringToObject(v, "tuya_client_secret", sec);
    cJSON_AddBoolToObject(v, "tuya_secret_set", sec[0] != '\0');
    return v;
}

// ---------- Public JSON APIs ----------
char *tuya_dev_status_json(tuya_dev_kind_t kind) {
    td_init_once();
    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    char def_reg[8], def_cid[48], def_sec[64];
    td_any_creds(def_reg, sizeof(def_reg), def_cid, sizeof(def_cid), def_sec, sizeof(def_sec));
    cJSON *arr = cJSON_Duplicate(cJSON_GetObjectItem(s_store, kind_list_key(kind)), true);
    const char *sk = kind_sel_key(kind);
    char sel_id[48] = {0};
    if (sk[0]) strncpy(sel_id, jstr(s_store, sk, ""), sizeof(sel_id) - 1);
    xSemaphoreGive(s_td_mutex);

    if (!sel_id[0] && cJSON_GetArraySize(arr) > 0) {
        strncpy(sel_id, jstr(cJSON_GetArrayItem(arr, 0), "id", ""), sizeof(sel_id) - 1);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "tuya_default_region", def_reg);
    cJSON_AddStringToObject(resp, "tuya_default_client_id", def_cid);
    cJSON_AddStringToObject(resp, "tuya_default_client_secret", def_sec);

    if (kind == TUYA_KIND_SENSOR) {
        cJSON *out_devs = cJSON_CreateArray();
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, arr) {
            cJSON *view = safe_dev_view(d, def_reg, def_cid, def_sec, "Senzor");
            cJSON *dps = NULL;
            char used[16] = {0}, err[128] = {0};
            bool ok = dev_read_dps(d, &dps, used, sizeof(used), err, sizeof(err));
            cJSON_AddStringToObject(view, "conn_used", used);
            cJSON_AddBoolToObject(view, "connected", ok);
            if (ok && dps) {
                cJSON_AddItemToObject(view, "state", dps_to_state(kind, dps, jstr(d, "id", "")));
                cJSON_AddStringToObject(view, "error", "");
                cJSON_Delete(dps);
            } else {
                cJSON_AddNullToObject(view, "state");
                cJSON_AddStringToObject(view, "error", err);
            }
            cJSON_AddItemToArray(out_devs, view);
        }
        cJSON_AddItemToObject(resp, "devices", out_devs);
    } else {
        cJSON *safe_devs = cJSON_CreateArray();
        cJSON *cur_dev = NULL, *d = NULL;
        cJSON_ArrayForEach(d, arr) {
            cJSON_AddItemToArray(safe_devs, safe_dev_view(d, def_reg, def_cid, def_sec, kind_fallback_name(kind)));
            if (strcmp(jstr(d, "id", ""), sel_id) == 0) cur_dev = d;
        }
        cJSON_AddItemToObject(resp, "devices", safe_devs);
        cJSON_AddStringToObject(resp, "selected_device_id", sel_id);
        cJSON_AddStringToObject(resp, "conn_mode", cur_dev ? jstr(cur_dev, "conn_mode", "auto") : "auto");
        if (cur_dev) {
            cJSON *dps = NULL;
            char used[16] = {0}, err[128] = {0};
            bool ok = dev_read_dps(cur_dev, &dps, used, sizeof(used), err, sizeof(err));
            cJSON_AddStringToObject(resp, "conn_used", used);
            cJSON_AddBoolToObject(resp, "connected", ok);
            if (ok && dps) {
                cJSON *st = dps_to_state(kind, dps, sel_id);
                xSemaphoreTake(s_td_mutex, portMAX_DELAY);
                char ck[64];
                snprintf(ck, sizeof(ck), "%s:%s", kind_list_key(kind), sel_id);
                cJSON_DeleteItemFromObject(s_cache, ck);
                cJSON_AddItemToObject(s_cache, ck, cJSON_Duplicate(st, true));
                xSemaphoreGive(s_td_mutex);
                cJSON_AddItemToObject(resp, "state", st);
                cJSON_AddStringToObject(resp, "error", "");
                cJSON_Delete(dps);
            } else {
                cJSON_AddNullToObject(resp, "state");
                cJSON_AddStringToObject(resp, "error", err);
            }
        } else {
            cJSON_AddStringToObject(resp, "conn_used", "");
            cJSON_AddBoolToObject(resp, "connected", false);
            cJSON_AddNullToObject(resp, "state");
            cJSON_AddStringToObject(resp, "error", "");
        }
    }
    cJSON_Delete(arr);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}

char *tuya_dev_discover_json(tuya_dev_kind_t kind, const char *req_body) {
    td_init_once();
    cJSON *in = req_body && req_body[0] ? cJSON_Parse(req_body) : NULL;
    char reg[8], cid[48], sec[64];
    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    td_any_creds(reg, sizeof(reg), cid, sizeof(cid), sec, sizeof(sec));
    xSemaphoreGive(s_td_mutex);
    if (jstr(in, "tuya_region", "")[0]) strncpy(reg, jstr(in, "tuya_region", ""), sizeof(reg) - 1);
    if (jstr(in, "tuya_client_id", "")[0]) strncpy(cid, jstr(in, "tuya_client_id", ""), sizeof(cid) - 1);
    if (jstr(in, "tuya_client_secret", "")[0]) strncpy(sec, jstr(in, "tuya_client_secret", ""), sizeof(sec) - 1);
    if (in) cJSON_Delete(in);

    if (!cid[0] || !sec[0]) return strdup("{\"ok\":false,\"error\":\"Introdu Access ID si Access Secret Tuya.\"}");

    char err[128] = {0};
    if (!ac_tuya_get_token(reg, cid, sec, false, err, sizeof(err))) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "error", err[0] ? err : "Token Tuya esuat");
        char *out = cJSON_PrintUnformatted(e); cJSON_Delete(e); return out;
    }
    char *resp = malloc(8192);
    if (!resp) return strdup("{\"ok\":false,\"error\":\"Memorie insuficienta\"}");
    int code = ac_tuya_signed_req(reg, cid, sec, HTTP_METHOD_GET, "/v1.0/iot-01/associated-users/devices", NULL, resp, 8192);
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (code != 200 || !j || !cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
        cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "error", cJSON_IsString(m) ? m->valuestring : "Eroare listare Tuya");
        if (j) cJSON_Delete(j);
        char *out = cJSON_PrintUnformatted(e); cJSON_Delete(e); return out;
    }

    cJSON *res_o = cJSON_GetObjectItem(j, "result");
    cJSON *raw = res_o ? cJSON_GetObjectItem(res_o, "devices") : NULL;
    lan_disc_entry_t lan_entries[8];
    int lan_cnt = 0;
    tuya_lan_udp_scan(lan_entries, 8, &lan_cnt, 3000);

    cJSON *out_arr = cJSON_CreateArray();
    for (int pass = 0; pass < 2 && cJSON_GetArraySize(out_arr) == 0; pass++) {
        cJSON *d = NULL;
        cJSON_ArrayForEach(d, raw) {
            const char *did = jstr(d, "id", "");
            const char *cat = jstr(d, "category", "");
            if (!did[0]) continue;
            bool match = (pass == 1);
            if (pass == 0) {
                if (kind == TUYA_KIND_DEHUM) match = (strcmp(cat, "cs") == 0 || strcmp(cat, "kj") == 0);
                else if (kind == TUYA_KIND_SENSOR) match = (strcmp(cat, "wsdcg") == 0);
                else match = (strcmp(cat, "cz") == 0 || strcmp(cat, "pc") == 0 || strcmp(cat, "kg") == 0 || strcmp(cat, "tdq") == 0);
            }
            if (!match) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", did);
            cJSON_AddStringToObject(o, "name", jstr(d, "name", kind_fallback_name(kind)));
            cJSON_AddStringToObject(o, "local_key", jstr(d, "local_key", ""));
            const char *ip = "", *ver = "auto";
            for (int k = 0; k < lan_cnt; k++) {
                if (strcmp(lan_entries[k].id, did) == 0) { ip = lan_entries[k].ip; ver = lan_entries[k].ver; break; }
            }
            cJSON_AddStringToObject(o, "ip", ip);
            cJSON_AddStringToObject(o, "version", ver);
            cJSON_AddItemToArray(out_arr, o);
        }
    }
    cJSON_Delete(j);

    cJSON *ok_r = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok_r, "ok", true);
    cJSON_AddItemToObject(ok_r, "devices", out_arr);
    cJSON_AddStringToObject(ok_r, "tuya_region", reg);
    cJSON_AddStringToObject(ok_r, "tuya_client_id", cid);
    cJSON_AddStringToObject(ok_r, "tuya_client_secret", sec);
    char *out = cJSON_PrintUnformatted(ok_r);
    cJSON_Delete(ok_r);
    return out;
}

char *tuya_dev_config_json(tuya_dev_kind_t kind, const char *req_body) {
    td_init_once();
    cJSON *in = req_body ? cJSON_Parse(req_body) : NULL;
    if (!in) return strdup("{\"ok\":false,\"error\":\"JSON invalid\"}");

    const char *lk_key = kind_list_key(kind);
    const char *sk_key = kind_sel_key(kind);

    if (cJSON_IsTrue(cJSON_GetObjectItem(in, "disconnect"))) {
        const char *tid = jstr(in, "device_id", "");
        xSemaphoreTake(s_td_mutex, portMAX_DELAY);
        cJSON *arr = cJSON_GetObjectItem(s_store, lk_key);
        cJSON *next = cJSON_CreateArray();
        if (tid[0]) {
            cJSON *d = NULL;
            cJSON_ArrayForEach(d, arr) {
                if (strcmp(jstr(d, "id", ""), tid) != 0) cJSON_AddItemToArray(next, cJSON_Duplicate(d, true));
            }
        }
        cJSON_ReplaceItemInObject(s_store, lk_key, next);
        if (sk_key[0]) {
            const char *first_id = (cJSON_GetArraySize(next) > 0) ? jstr(cJSON_GetArrayItem(next, 0), "id", "") : "";
            cJSON_DeleteItemFromObject(s_store, sk_key);
            cJSON_AddStringToObject(s_store, sk_key, first_id);
        }
        td_save_store();
        xSemaphoreGive(s_td_mutex);
        cJSON_Delete(in);
        return tuya_dev_status_json(kind);
    }

    const char *did = jstr(in, "device_id", "");
    if (!did[0]) { cJSON_Delete(in); return strdup("{\"ok\":false,\"error\":\"Introdu Device ID.\"}"); }

    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "id", did);
    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    cJSON *arr = cJSON_GetObjectItem(s_store, lk_key);
    cJSON *existing = NULL, *d = NULL;
    cJSON_ArrayForEach(d, arr) {
        if (strcmp(jstr(d, "id", ""), did) == 0) { existing = d; break; }
    }
    const char *nm = jstr(in, "name", existing ? jstr(existing, "name", kind_fallback_name(kind)) : kind_fallback_name(kind));
    const char *lkey = jstr(in, "local_key", existing ? jstr(existing, "local_key", "") : "");
    const char *sec = jstr(in, "tuya_client_secret", existing ? jstr(existing, "tuya_client_secret", "") : "");
    cJSON_AddStringToObject(entry, "name", nm);
    cJSON_AddStringToObject(entry, "conn_mode", jstr(in, "conn_mode", "auto"));
    cJSON_AddStringToObject(entry, "ip", jstr(in, "ip", ""));
    cJSON_AddStringToObject(entry, "version", jstr(in, "version", "auto"));
    cJSON_AddStringToObject(entry, "local_key", lkey);
    cJSON_AddStringToObject(entry, "tuya_region", jstr(in, "tuya_region", ""));
    cJSON_AddStringToObject(entry, "tuya_client_id", jstr(in, "tuya_client_id", ""));
    cJSON_AddStringToObject(entry, "tuya_client_secret", sec);
    xSemaphoreGive(s_td_mutex);
    cJSON_Delete(in);

    cJSON *dps = NULL;
    char used[16] = {0}, err[128] = {0};
    if (!dev_read_dps(entry, &dps, used, sizeof(used), err, sizeof(err))) {
        cJSON_Delete(entry);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "error", err[0] ? err : "Conectare esuata");
        char *out = cJSON_PrintUnformatted(e); cJSON_Delete(e); return out;
    }
    if (dps) cJSON_Delete(dps);

    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    arr = cJSON_GetObjectItem(s_store, lk_key);
    cJSON *next = cJSON_CreateArray();
    cJSON_ArrayForEach(d, arr) {
        if (strcmp(jstr(d, "id", ""), did) != 0) cJSON_AddItemToArray(next, cJSON_Duplicate(d, true));
    }
    cJSON_AddItemToArray(next, entry);
    cJSON_ReplaceItemInObject(s_store, lk_key, next);
    if (sk_key[0]) {
        cJSON_DeleteItemFromObject(s_store, sk_key);
        cJSON_AddStringToObject(s_store, sk_key, did);
    }
    td_save_store();
    xSemaphoreGive(s_td_mutex);
    return tuya_dev_status_json(kind);
}

char *tuya_dev_control_json(tuya_dev_kind_t kind, const char *req_body) {
    td_init_once();
    cJSON *in = req_body ? cJSON_Parse(req_body) : NULL;
    if (!in) return strdup("{\"ok\":false,\"error\":\"JSON invalid\"}");

    const char *lk_key = kind_list_key(kind);
    const char *sk_key = kind_sel_key(kind);
    const char *req_did = jstr(in, "device_id", "");
    const char *new_cm = jstr(in, "conn_mode", "");

    xSemaphoreTake(s_td_mutex, portMAX_DELAY);
    char sel_id[48] = {0};
    if (req_did[0]) strncpy(sel_id, req_did, sizeof(sel_id) - 1);
    else if (sk_key[0]) strncpy(sel_id, jstr(s_store, sk_key, ""), sizeof(sel_id) - 1);

    bool dirty = false;
    if (sk_key[0] && sel_id[0] && strcmp(sel_id, jstr(s_store, sk_key, "")) != 0) {
        cJSON_DeleteItemFromObject(s_store, sk_key);
        cJSON_AddStringToObject(s_store, sk_key, sel_id);
        dirty = true;
    }
    cJSON *arr = cJSON_GetObjectItem(s_store, lk_key);
    cJSON *cur_dev = NULL, *d = NULL;
    cJSON_ArrayForEach(d, arr) {
        if (strcmp(jstr(d, "id", ""), sel_id) == 0) { cur_dev = d; break; }
    }
    if (cur_dev && (strcmp(new_cm, "auto") == 0 || strcmp(new_cm, "lan") == 0 || strcmp(new_cm, "cloud") == 0)) {
        cJSON_DeleteItemFromObject(cur_dev, "conn_mode");
        cJSON_AddStringToObject(cur_dev, "conn_mode", new_cm);
        dirty = true;
    }
    if (dirty) td_save_store();
    cJSON *dev_copy = cur_dev ? cJSON_Duplicate(cur_dev, true) : NULL;
    xSemaphoreGive(s_td_mutex);

    if (kind == TUYA_KIND_SENSOR) {
        cJSON_Delete(in);
        if (dev_copy) cJSON_Delete(dev_copy);
        return tuya_dev_status_json(kind);
    }
    if (!dev_copy) {
        cJSON_Delete(in);
        return strdup("{\"ok\":false,\"error\":\"Dispozitiv neconfigurat.\"}");
    }

    cJSON *lan_dps = cJSON_CreateObject();
    cJSON *cloud_props = cJSON_CreateObject();
    bool has_cmd = false;
    if (kind == TUYA_KIND_DEHUM) {
        cJSON *p = cJSON_GetObjectItem(in, "power");
        cJSON *th = cJSON_GetObjectItem(in, "target_humidity");
        cJSON *fs = cJSON_GetObjectItem(in, "fan_speed");
        cJSON *md = cJSON_GetObjectItem(in, "mode");
        cJSON *an = cJSON_GetObjectItem(in, "anion");
        cJSON *lk = cJSON_GetObjectItem(in, "child_lock");
        if (cJSON_IsBool(p)) { has_cmd = true; cJSON_AddBoolToObject(lan_dps, "1", cJSON_IsTrue(p)); cJSON_AddBoolToObject(cloud_props, "switch", cJSON_IsTrue(p)); }
        if (cJSON_IsNumber(th)) {
            has_cmd = true;
            int hv = ((th->valueint + 2) / 5) * 5;
            if (hv < 30) hv = 30; if (hv > 80) hv = 80;
            char hb[8]; snprintf(hb, sizeof(hb), "%d", hv);
            cJSON_AddStringToObject(lan_dps, "3", hb); cJSON_AddStringToObject(cloud_props, "dehumidify_set_enum", hb);
        }
        if (cJSON_IsString(fs) && fs->valuestring[0]) { has_cmd = true; cJSON_AddStringToObject(lan_dps, "4", fs->valuestring); cJSON_AddStringToObject(cloud_props, "fan_speed_enum", fs->valuestring); }
        if (cJSON_IsString(md) && md->valuestring[0]) { has_cmd = true; cJSON_AddStringToObject(lan_dps, "5", md->valuestring); cJSON_AddStringToObject(cloud_props, "mode", md->valuestring); }
        if (cJSON_IsBool(an)) { has_cmd = true; cJSON_AddBoolToObject(lan_dps, "10", cJSON_IsTrue(an)); cJSON_AddBoolToObject(cloud_props, "anion", cJSON_IsTrue(an)); }
        if (cJSON_IsBool(lk)) { has_cmd = true; cJSON_AddBoolToObject(lan_dps, "16", cJSON_IsTrue(lk)); cJSON_AddBoolToObject(cloud_props, "child_lock", cJSON_IsTrue(lk)); }
    } else {
        cJSON *p = cJSON_GetObjectItem(in, "power");
        if (cJSON_IsBool(p)) { has_cmd = true; cJSON_AddBoolToObject(lan_dps, "1", cJSON_IsTrue(p)); cJSON_AddBoolToObject(cloud_props, "switch_1", cJSON_IsTrue(p)); }
    }

    char used[16] = {0}, err[128] = {0};
    bool ok = false;
    cJSON *out_state = NULL;

    if (has_cmd) {
        const char *did = jstr(dev_copy, "id", "");
        const char *mode = jstr(dev_copy, "conn_mode", "auto");
        const char *ip = jstr(dev_copy, "ip", "");
        const char *lkey = jstr(dev_copy, "local_key", "");
        const char *ver = jstr(dev_copy, "version", "auto");
        char reg[8], cid[48], sec[64];
        td_eff_creds(dev_copy, reg, sizeof(reg), cid, sizeof(cid), sec, sizeof(sec));

        if ((strcmp(mode, "lan") == 0 || strcmp(mode, "auto") == 0) && ip[0] && lkey[0]) {
            char *dstr = cJSON_PrintUnformatted(lan_dps);
            cJSON *rx_dps = NULL;
            ok = tuya_lan_req(did, ip, lkey, ver, dstr, &rx_dps, err, sizeof(err));
            free(dstr);
            if (rx_dps) cJSON_Delete(rx_dps);
            if (ok) strncpy(used, "lan", sizeof(used) - 1);
        }
        if (!ok && (strcmp(mode, "cloud") == 0 || strcmp(mode, "auto") == 0) && cid[0] && sec[0]) {
            ok = tuya_cloud_write_props(reg, cid, sec, did, cloud_props, err, sizeof(err));
            if (ok) strncpy(used, "cloud", sizeof(used) - 1);
        }
        if (ok) {
            xSemaphoreTake(s_td_mutex, portMAX_DELAY);
            char ck[64];
            snprintf(ck, sizeof(ck), "%s:%s", lk_key, sel_id);
            cJSON *cached = cJSON_GetObjectItem(s_cache, ck);
            out_state = cached ? cJSON_Duplicate(cached, true) : dps_to_state(kind, cloud_props, sel_id);
            cJSON *it = NULL;
            cJSON_ArrayForEach(it, cloud_props) {
                if (strcmp(it->string, "switch") == 0 || strcmp(it->string, "switch_1") == 0)
                    cJSON_ReplaceItemInObject(out_state, "power", cJSON_Duplicate(it, true));
                else if (strcmp(it->string, "dehumidify_set_enum") == 0) {
                    cJSON_DeleteItemFromObject(out_state, "target_humidity");
                    cJSON_AddNumberToObject(out_state, "target_humidity", atoi(it->valuestring));
                } else {
                    cJSON_DeleteItemFromObject(out_state, it->string);
                    cJSON_AddItemToObject(out_state, it->string, cJSON_Duplicate(it, true));
                }
            }
            cJSON_DeleteItemFromObject(s_cache, ck);
            cJSON_AddItemToObject(s_cache, ck, cJSON_Duplicate(out_state, true));
            xSemaphoreGive(s_td_mutex);
        }
    } else {
        cJSON *dps = NULL;
        ok = dev_read_dps(dev_copy, &dps, used, sizeof(used), err, sizeof(err));
        if (ok && dps) { out_state = dps_to_state(kind, dps, sel_id); cJSON_Delete(dps); }
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (ok && out_state) {
        cJSON_AddItemToObject(resp, "state", out_state);
        cJSON_AddStringToObject(resp, "conn_mode", jstr(dev_copy, "conn_mode", "auto"));
        cJSON_AddStringToObject(resp, "conn_used", used);
        cJSON_AddStringToObject(resp, "selected_device_id", sel_id);
    } else {
        if (out_state) cJSON_Delete(out_state);
        cJSON_AddStringToObject(resp, "error", err[0] ? err : "Eroare comanda");
    }
    cJSON_Delete(lan_dps);
    cJSON_Delete(cloud_props);
    cJSON_Delete(in);
    cJSON_Delete(dev_copy);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out;
}
