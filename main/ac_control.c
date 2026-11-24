#include "ac_control.h"
#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "cJSON.h"

static const char *TAG = "AC_CTRL";

#define TCL_LOGIN_URL  "https://pa.account.tcl.com/account/login?clientId=54148614"
#define TCL_CLOUD_URL  "https://prod-center.aws.tcljd.com/v3/global/cloud_url_get"
#define TCL_APP_ID     "wx6e1af3fa84fbe523"

static SemaphoreHandle_t s_ac_mutex = NULL;

typedef struct {
    bool valid;
    char email[96];
    char pw_md5[33];
    char device_url[160];
    char cloud_region[32];
    char saas_token[1024];
    char iot_host[96];
    char ak[128];
    char sk[128];
    char st[2400];
    time_t aws_exp;
} tcl_session_t;

typedef struct {
    bool valid;
    char cid[48];
    char reg[8];
    char token[96];
    time_t exp;
} tuya_session_t;

static tcl_session_t s_tcl_sess = {0};
static tuya_session_t s_tuya_sess = {0};
static ac_state_t s_last_state = {
    .valid = false, .power = false, .target_temp = 23,
    .has_current_temp = false, .current_temp = 0.0f,
    .mode = "cool", .fan_speed = 0, .fan_max = 7, .fan_type = "standard"
};

void ac_control_init(void) {
    if (!s_ac_mutex) {
        s_ac_mutex = xSemaphoreCreateMutex();
    }
}

void ac_control_clear_cache(void) {
    if (s_ac_mutex) xSemaphoreTake(s_ac_mutex, portMAX_DELAY);
    memset(&s_tcl_sess, 0, sizeof(s_tcl_sess));
    memset(&s_tuya_sess, 0, sizeof(s_tuya_sess));
    memset(&s_last_state, 0, sizeof(s_last_state));
    s_last_state.target_temp = 23;
    strcpy(s_last_state.mode, "cool");
    s_last_state.fan_max = 7;
    strcpy(s_last_state.fan_type, "standard");
    if (s_ac_mutex) xSemaphoreGive(s_ac_mutex);
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out, bool upper) {
    const char *hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static void calc_md5_hex(const char *str, char *out33) {
    uint8_t digest[16] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    mbedtls_md(info, (const unsigned char *)str, strlen(str), digest);
    bytes_to_hex(digest, 16, out33, false);
}

static void calc_sha256_hex(const uint8_t *data, size_t len, char *out65) {
    uint8_t digest[32] = {0};
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md(info, data, len, digest);
    bytes_to_hex(digest, 32, out65, false);
}

static void calc_hmac_sha256(const uint8_t *key, size_t key_len,
                             const uint8_t *msg, size_t msg_len, uint8_t out32[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(info, key, key_len, msg, msg_len, out32);
}

static void url_encode(const char *src, char *dst, size_t dst_sz, const char *safe_chars) {
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 4 < dst_sz; i++) {
        unsigned char c = (unsigned char)src[i];
        bool is_alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        bool is_unreserved = (c == '-' || c == '_' || c == '.' || c == '~');
        bool is_safe = (safe_chars && strchr(safe_chars, (int)c) != NULL);
        if (is_alnum || is_unreserved || is_safe) {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0xF];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
}

typedef struct {
    const char *k;
    const char *v;
} hdr_kv_t;

static int http_req_json(const char *url, esp_http_client_method_t method,
                         const hdr_kv_t *hdrs, int nhdrs,
                         const char *body, char *resp_buf, size_t resp_sz) {
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;

    for (int i = 0; i < nhdrs; i++) {
        if (hdrs[i].k && hdrs[i].v && hdrs[i].v[0]) {
            esp_http_client_set_header(client, hdrs[i].k, hdrs[i].v);
        }
    }

    int body_len = body ? (int)strlen(body) : 0;
    if (esp_http_client_open(client, body_len) != ESP_OK) {
        esp_http_client_cleanup(client);
        return 0;
    }
    if (body_len > 0) {
        esp_http_client_write(client, body, body_len);
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    size_t total = 0;
    while (total + 1 < resp_sz) {
        int r = esp_http_client_read(client, resp_buf + total, (int)(resp_sz - 1 - total));
        if (r <= 0) break;
        total += (size_t)r;
    }
    resp_buf[total] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

static bool jwt_get_sub(const char *jwt, char *sub_out, size_t sub_sz) {
    const char *p1 = strchr(jwt, '.');
    if (!p1) return false;
    p1++;
    const char *p2 = strchr(p1, '.');
    if (!p2) return false;
    size_t b64len = (size_t)(p2 - p1);
    if (b64len == 0 || b64len > 1024) return false;

    char b64[1040];
    memcpy(b64, p1, b64len);
    for (size_t i = 0; i < b64len; i++) {
        if (b64[i] == '-') b64[i] = '+';
        else if (b64[i] == '_') b64[i] = '/';
    }
    while (b64len % 4 != 0) b64[b64len++] = '=';
    b64[b64len] = '\0';

    unsigned char dec[768] = {0};
    size_t olen = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &olen, (const unsigned char *)b64, b64len) != 0) {
        return false;
    }
    dec[olen] = '\0';
    cJSON *root = cJSON_Parse((const char *)dec);
    if (!root) return false;
    cJSON *sub = cJSON_GetObjectItem(root, "sub");
    bool ok = (cJSON_IsString(sub) && sub->valuestring);
    if (ok) {
        strncpy(sub_out, sub->valuestring, sub_sz - 1);
        sub_out[sub_sz - 1] = '\0';
    }
    cJSON_Delete(root);
    return ok;
}

static bool tcl_ensure_session(const char *email, const char *password, bool force, char *err_buf, size_t err_sz) {
    time_t now = time(NULL);
    char pw_md5[33];
    calc_md5_hex(password, pw_md5);

    if (!force && s_tcl_sess.valid &&
        strcmp(s_tcl_sess.email, email) == 0 &&
        strcmp(s_tcl_sess.pw_md5, pw_md5) == 0 &&
        now < s_tcl_sess.aws_exp - 120) {
        return true;
    }

    char *resp = malloc(8192);
    if (!resp) {
        snprintf(err_buf, err_sz, "Memorie insuficienta");
        return false;
    }

    cJSON *auth_p = cJSON_CreateObject();
    cJSON_AddNumberToObject(auth_p, "equipment", 2);
    cJSON_AddStringToObject(auth_p, "password", pw_md5);
    cJSON_AddNumberToObject(auth_p, "osType", 1);
    cJSON_AddStringToObject(auth_p, "username", email);
    cJSON_AddStringToObject(auth_p, "clientVersion", "4.8.1");
    cJSON_AddStringToObject(auth_p, "osVersion", "6.0");
    cJSON_AddStringToObject(auth_p, "deviceModel", "AndroidAndroid SDK built for x86");
    cJSON_AddNumberToObject(auth_p, "captchaRule", 2);
    cJSON_AddStringToObject(auth_p, "channel", "app");
    char *auth_body = cJSON_PrintUnformatted(auth_p);
    cJSON_Delete(auth_p);

    hdr_kv_t auth_hdrs[] = {
        {"th_platform", "android"},
        {"th_version", "4.8.1"},
        {"th_appbulid", "830"},
        {"user-agent", "Android"},
        {"content-type", "application/json; charset=UTF-8"},
    };
    int code = http_req_json(TCL_LOGIN_URL, HTTP_METHOD_POST, auth_hdrs, 5, auth_body, resp, 8192);
    free(auth_body);

    cJSON *auth_j = cJSON_Parse(resp);
    int st_val = auth_j ? (cJSON_GetObjectItem(auth_j, "status") ? cJSON_GetObjectItem(auth_j, "status")->valueint : 0) : 0;
    if (code != 200 || st_val != 1) {
        const char *msg = NULL;
        if (auth_j) {
            cJSON *m = cJSON_GetObjectItem(auth_j, "msg");
            if (!cJSON_IsString(m)) m = cJSON_GetObjectItem(auth_j, "message");
            if (cJSON_IsString(m)) msg = m->valuestring;
        }
        snprintf(err_buf, err_sz, "%s", msg ? msg : "Autentificare TCL esuata");
        if (auth_j) cJSON_Delete(auth_j);
        free(resp);
        return false;
    }

    char sso_token[512] = {0}, username[128] = {0};
    cJSON *tok_j = cJSON_GetObjectItem(auth_j, "token");
    cJSON *usr_j = cJSON_GetObjectItem(auth_j, "user");
    cJSON *un_j = usr_j ? cJSON_GetObjectItem(usr_j, "username") : NULL;
    if (cJSON_IsString(tok_j)) strncpy(sso_token, tok_j->valuestring, sizeof(sso_token) - 1);
    if (cJSON_IsString(un_j)) strncpy(username, un_j->valuestring, sizeof(username) - 1);
    cJSON_Delete(auth_j);

    // 2. Get cloud URLs
    cJSON *url_p = cJSON_CreateObject();
    cJSON_AddStringToObject(url_p, "ssoId", username);
    cJSON_AddStringToObject(url_p, "ssoToken", sso_token);
    char *url_body = cJSON_PrintUnformatted(url_p);
    cJSON_Delete(url_p);
    hdr_kv_t std_hdrs[] = {
        {"user-agent", "Android"},
        {"content-type", "application/json; charset=UTF-8"},
    };
    code = http_req_json(TCL_CLOUD_URL, HTTP_METHOD_POST, std_hdrs, 2, url_body, resp, 8192);
    free(url_body);

    cJSON *url_j = cJSON_Parse(resp);
    cJSON *udata = url_j ? cJSON_GetObjectItem(url_j, "data") : NULL;
    char cloud_url[160] = {0}, device_url[160] = {0}, region[32] = "eu-central-1";
    if (code == 200 && udata) {
        cJSON *cu = cJSON_GetObjectItem(udata, "cloud_url");
        cJSON *du = cJSON_GetObjectItem(udata, "device_url");
        cJSON *rg = cJSON_GetObjectItem(udata, "cloud_region");
        if (cJSON_IsString(cu)) strncpy(cloud_url, cu->valuestring, sizeof(cloud_url) - 1);
        if (cJSON_IsString(du)) strncpy(device_url, du->valuestring, sizeof(device_url) - 1);
        if (cJSON_IsString(rg) && rg->valuestring[0]) strncpy(region, rg->valuestring, sizeof(region) - 1);
    }
    if (url_j) cJSON_Delete(url_j);
    if (!cloud_url[0] || !device_url[0]) {
        snprintf(err_buf, err_sz, "Eroare servere cloud TCL");
        free(resp);
        return false;
    }

    // 3. Refresh tokens
    char ref_url[200];
    snprintf(ref_url, sizeof(ref_url), "%s/v3/auth/refresh_tokens", cloud_url);
    cJSON *ref_p = cJSON_CreateObject();
    cJSON_AddStringToObject(ref_p, "userId", username);
    cJSON_AddStringToObject(ref_p, "ssoToken", sso_token);
    cJSON_AddStringToObject(ref_p, "appId", TCL_APP_ID);
    char *ref_body = cJSON_PrintUnformatted(ref_p);
    cJSON_Delete(ref_p);
    code = http_req_json(ref_url, HTTP_METHOD_POST, std_hdrs, 2, ref_body, resp, 8192);
    free(ref_body);

    cJSON *ref_j = cJSON_Parse(resp);
    cJSON *rdata = ref_j ? cJSON_GetObjectItem(ref_j, "data") : NULL;
    char saas_tok[1024] = {0}, cog_tok[2048] = {0};
    if (code == 200 && rdata) {
        cJSON *st = cJSON_GetObjectItem(rdata, "saas_token");
        if (!cJSON_IsString(st)) st = cJSON_GetObjectItem(rdata, "saasToken");
        cJSON *ct = cJSON_GetObjectItem(rdata, "cognito_token");
        if (!cJSON_IsString(ct)) ct = cJSON_GetObjectItem(rdata, "cognitoToken");
        if (cJSON_IsString(st)) strncpy(saas_tok, st->valuestring, sizeof(saas_tok) - 1);
        if (cJSON_IsString(ct)) strncpy(cog_tok, ct->valuestring, sizeof(cog_tok) - 1);
    }
    if (ref_j) cJSON_Delete(ref_j);
    if (!saas_tok[0] || !cog_tok[0]) {
        snprintf(err_buf, err_sz, "Refresh token TCL esuat");
        free(resp);
        return false;
    }

    // 4. AWS Cognito credentials
    char identity_id[128] = {0};
    if (!jwt_get_sub(cog_tok, identity_id, sizeof(identity_id))) {
        snprintf(err_buf, err_sz, "Token Cognito invalid");
        free(resp);
        return false;
    }
    char cog_url[96];
    snprintf(cog_url, sizeof(cog_url), "https://cognito-identity.%s.amazonaws.com/", region);
    cJSON *cog_p = cJSON_CreateObject();
    cJSON_AddStringToObject(cog_p, "IdentityId", identity_id);
    cJSON *logins = cJSON_AddObjectToObject(cog_p, "Logins");
    cJSON_AddStringToObject(logins, "cognito-identity.amazonaws.com", cog_tok);
    char *cog_body = cJSON_PrintUnformatted(cog_p);
    cJSON_Delete(cog_p);

    hdr_kv_t cog_hdrs[] = {
        {"User-agent", "aws-sdk-android/2.22.6"},
        {"X-Amz-Target", "AWSCognitoIdentityService.GetCredentialsForIdentity"},
        {"content-type", "application/x-amz-json-1.1"},
    };
    code = http_req_json(cog_url, HTTP_METHOD_POST, cog_hdrs, 3, cog_body, resp, 8192);
    free(cog_body);

    cJSON *cog_j = cJSON_Parse(resp);
    cJSON *creds = cog_j ? cJSON_GetObjectItem(cog_j, "Credentials") : NULL;
    bool ok_aws = false;
    if (code == 200 && creds) {
        cJSON *ak = cJSON_GetObjectItem(creds, "AccessKeyId");
        cJSON *sk = cJSON_GetObjectItem(creds, "SecretKey");
        cJSON *st = cJSON_GetObjectItem(creds, "SessionToken");
        cJSON *ex = cJSON_GetObjectItem(creds, "Expiration");
        if (cJSON_IsString(ak) && cJSON_IsString(sk) && cJSON_IsString(st)) {
            memset(&s_tcl_sess, 0, sizeof(s_tcl_sess));
            s_tcl_sess.valid = true;
            strncpy(s_tcl_sess.email, email, sizeof(s_tcl_sess.email) - 1);
            strncpy(s_tcl_sess.pw_md5, pw_md5, sizeof(s_tcl_sess.pw_md5) - 1);
            strncpy(s_tcl_sess.device_url, device_url, sizeof(s_tcl_sess.device_url) - 1);
            strncpy(s_tcl_sess.cloud_region, region, sizeof(s_tcl_sess.cloud_region) - 1);
            strncpy(s_tcl_sess.saas_token, saas_tok, sizeof(s_tcl_sess.saas_token) - 1);
            snprintf(s_tcl_sess.iot_host, sizeof(s_tcl_sess.iot_host), "data-ats.iot.%s.amazonaws.com", region);
            strncpy(s_tcl_sess.ak, ak->valuestring, sizeof(s_tcl_sess.ak) - 1);
            strncpy(s_tcl_sess.sk, sk->valuestring, sizeof(s_tcl_sess.sk) - 1);
            strncpy(s_tcl_sess.st, st->valuestring, sizeof(s_tcl_sess.st) - 1);
            s_tcl_sess.aws_exp = cJSON_IsNumber(ex) ? (time_t)ex->valuedouble : (now + 3000);
            ok_aws = true;
        }
    }
    if (cog_j) cJSON_Delete(cog_j);
    if (!ok_aws) {
        snprintf(err_buf, err_sz, "AWS Cognito HTTP %d: %.80s", code, resp);
        free(resp);
        return false;
    }
    free(resp);
    return true;
}

static int tcl_shadow_req(const char *device_id, bool is_post, const char *body, char *resp_buf, size_t resp_sz) {
    char encoded_path[192];
    char query_str[16] = "";
    if (is_post) {
        char topic[128], enc_topic[160];
        snprintf(topic, sizeof(topic), "$aws/things/%s/shadow/update", device_id);
        url_encode(topic, enc_topic, sizeof(enc_topic), NULL);
        snprintf(encoded_path, sizeof(encoded_path), "/topics/%s", enc_topic);
        strcpy(query_str, "qos=1");
    } else {
        char enc_id[96];
        url_encode(device_id, enc_id, sizeof(enc_id), NULL);
        snprintf(encoded_path, sizeof(encoded_path), "/things/%s/shadow", enc_id);
    }

    char canonical_uri[256];
    url_encode(encoded_path, canonical_uri, sizeof(canonical_uri), "/-_.~");

    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    char amz_date[20], date_stamp[12];
    strftime(amz_date, sizeof(amz_date), "%Y%m%dT%H%M%SZ", &tm_utc);
    strftime(date_stamp, sizeof(date_stamp), "%Y%m%d", &tm_utc);

    char payload_hash[65];
    calc_sha256_hex((const uint8_t *)(body ? body : ""), body ? strlen(body) : 0, payload_hash);

    char *canon_req = malloc(3600);
    if (!canon_req) return 0;
    snprintf(canon_req, 3600,
             "%s\n%s\n%s\nhost:%s\nx-amz-date:%s\nx-amz-security-token:%s\n\nhost;x-amz-date;x-amz-security-token\n%s",
             is_post ? "POST" : "GET", canonical_uri, query_str,
             s_tcl_sess.iot_host, amz_date, s_tcl_sess.st, payload_hash);

    char req_hash[65];
    calc_sha256_hex((const uint8_t *)canon_req, strlen(canon_req), req_hash);
    free(canon_req);

    char scope[96], str_to_sign[256];
    snprintf(scope, sizeof(scope), "%s/%s/iotdata/aws4_request", date_stamp, s_tcl_sess.cloud_region);
    snprintf(str_to_sign, sizeof(str_to_sign), "AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, req_hash);

    char k_secret[140];
    snprintf(k_secret, sizeof(k_secret), "AWS4%s", s_tcl_sess.sk);
    uint8_t k_date[32], k_reg[32], k_svc[32], k_sign[32], sig[32];
    calc_hmac_sha256((const uint8_t *)k_secret, strlen(k_secret), (const uint8_t *)date_stamp, strlen(date_stamp), k_date);
    calc_hmac_sha256(k_date, 32, (const uint8_t *)s_tcl_sess.cloud_region, strlen(s_tcl_sess.cloud_region), k_reg);
    calc_hmac_sha256(k_reg, 32, (const uint8_t *)"iotdata", 7, k_svc);
    calc_hmac_sha256(k_svc, 32, (const uint8_t *)"aws4_request", 12, k_sign);
    calc_hmac_sha256(k_sign, 32, (const uint8_t *)str_to_sign, strlen(str_to_sign), sig);

    char sig_hex[65];
    bytes_to_hex(sig, 32, sig_hex, false);

    char auth_hdr[384], full_url[320];
    snprintf(auth_hdr, sizeof(auth_hdr),
             "AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=host;x-amz-date;x-amz-security-token, Signature=%s",
             s_tcl_sess.ak, scope, sig_hex);
    snprintf(full_url, sizeof(full_url), "https://%s%s%s%s",
             s_tcl_sess.iot_host, encoded_path, query_str[0] ? "?" : "", query_str);

    hdr_kv_t hdrs[] = {
        {"x-amz-date", amz_date},
        {"x-amz-security-token", s_tcl_sess.st},
        {"Authorization", auth_hdr},
        {"Content-Type", "application/json"},
    };
    return http_req_json(full_url, is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
                         hdrs, 4, body, resp_buf, resp_sz);
}

bool ac_control_connect_tcl(const char *email, const char *password,
                            char *devices_json, size_t devices_json_sz,
                            char *first_dev_id, size_t first_dev_id_sz,
                            char *err_buf, size_t err_sz) {
    ac_control_init();
    xSemaphoreTake(s_ac_mutex, portMAX_DELAY);

    if (!tcl_ensure_session(email, password, true, err_buf, err_sz)) {
        xSemaphoreGive(s_ac_mutex);
        return false;
    }

    char url[220];
    snprintf(url, sizeof(url), "%s/v3/user/get_things", s_tcl_sess.device_url);
    char ts[24];
    snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL) * 1000LL);
    char nonce[17];
    static const char alnum[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 16; i++) nonce[i] = alnum[esp_random() % 36];
    nonce[16] = '\0';

    char *sign_in = malloc(1200);
    char *resp = malloc(8192);
    if (!sign_in || !resp) {
        free(sign_in); free(resp);
        snprintf(err_buf, err_sz, "Memorie insuficienta");
        xSemaphoreGive(s_ac_mutex);
        return false;
    }
    snprintf(sign_in, 1200, "%s%s%s", ts, nonce, s_tcl_sess.saas_token);
    char sign[33];
    calc_md5_hex(sign_in, sign);
    free(sign_in);

    hdr_kv_t hdrs[] = {
        {"platform", "android"},
        {"appversion", "5.4.1"},
        {"thomeversion", "4.8.1"},
        {"accesstoken", s_tcl_sess.saas_token},
        {"accept-language", "en"},
        {"timestamp", ts},
        {"nonce", nonce},
        {"sign", sign},
        {"user-agent", "Android"},
        {"content-type", "application/json; charset=UTF-8"},
    };
    int code = http_req_json(url, HTTP_METHOD_POST, hdrs, 10, "{}", resp, 8192);
    cJSON *root = cJSON_Parse(resp);
    free(resp);

    cJSON *arr = root ? cJSON_GetObjectItem(root, "data") : NULL;
    if (code != 200 || !cJSON_IsArray(arr)) {
        if (root) cJSON_Delete(root);
        snprintf(err_buf, err_sz, "Eroare listare dispozitive TCL (HTTP %d)", code);
        xSemaphoreGive(s_ac_mutex);
        return false;
    }

    cJSON *out_arr = cJSON_CreateArray();
    first_dev_id[0] = '\0';
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        cJSON *id_j = cJSON_GetObjectItem(item, "device_id");
        if (!cJSON_IsString(id_j)) id_j = cJSON_GetObjectItem(item, "deviceId");
        if (!cJSON_IsString(id_j) || !id_j->valuestring[0]) continue;
        cJSON *nm_j = cJSON_GetObjectItem(item, "nick_name");
        if (!cJSON_IsString(nm_j)) nm_j = cJSON_GetObjectItem(item, "nickName");
        if (!cJSON_IsString(nm_j)) nm_j = cJSON_GetObjectItem(item, "device_name");
        if (!cJSON_IsString(nm_j)) nm_j = cJSON_GetObjectItem(item, "deviceName");
        cJSON *on_j = cJSON_GetObjectItem(item, "is_online");
        if (!on_j) on_j = cJSON_GetObjectItem(item, "isOnline");

        if (!first_dev_id[0]) {
            strncpy(first_dev_id, id_j->valuestring, first_dev_id_sz - 1);
            first_dev_id[first_dev_id_sz - 1] = '\0';
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", id_j->valuestring);
        cJSON_AddStringToObject(o, "name", (cJSON_IsString(nm_j) && nm_j->valuestring[0]) ? nm_j->valuestring : id_j->valuestring);
        bool is_on = true;
        if (on_j) {
            if (cJSON_IsBool(on_j)) is_on = cJSON_IsTrue(on_j);
            else if (cJSON_IsNumber(on_j)) is_on = (on_j->valueint == 1);
            else if (cJSON_IsString(on_j) && on_j->valuestring) {
                is_on = (strcmp(on_j->valuestring, "1") == 0 || strcmp(on_j->valuestring, "true") == 0);
            }
        }
        cJSON_AddBoolToObject(o, "online", is_on);
        cJSON_AddItemToArray(out_arr, o);
    }
    if (root) cJSON_Delete(root);
    char *printed = cJSON_PrintUnformatted(out_arr);
    cJSON_Delete(out_arr);
    if (printed) {
        strncpy(devices_json, printed, devices_json_sz - 1);
        devices_json[devices_json_sz - 1] = '\0';
        free(printed);
    } else {
        snprintf(devices_json, devices_json_sz, "[]");
    }

    xSemaphoreGive(s_ac_mutex);
    return true;
}

static bool tuya_get_token(const char *region, const char *cid, const char *sec, bool force, char *err_buf, size_t err_sz) {
    time_t now = time(NULL);
    if (!force && s_tuya_sess.valid &&
        strcmp(s_tuya_sess.cid, cid) == 0 &&
        strcmp(s_tuya_sess.reg, region) == 0 &&
        now < s_tuya_sess.exp - 60) {
        return true;
    }

    const char *base = "https://openapi.tuyaeu.com";
    if (strcmp(region, "us") == 0) base = "https://openapi.tuyaus.com";
    else if (strcmp(region, "cn") == 0) base = "https://openapi.tuyacn.com";
    else if (strcmp(region, "in") == 0) base = "https://openapi.tuyain.com";

    const char *path = "/v1.0/token?grant_type=1";
    char t_str[24];
    snprintf(t_str, sizeof(t_str), "%lld", (long long)now * 1000LL);
    char empty_hash[65];
    calc_sha256_hex((const uint8_t *)"", 0, empty_hash);

    char sign_str[320];
    snprintf(sign_str, sizeof(sign_str), "%s%sGET\n%s\n\n%s", cid, t_str, empty_hash, path);
    uint8_t sig[32];
    calc_hmac_sha256((const uint8_t *)sec, strlen(sec), (const uint8_t *)sign_str, strlen(sign_str), sig);
    char sig_hex[65];
    bytes_to_hex(sig, 32, sig_hex, true);

    char url[128];
    snprintf(url, sizeof(url), "%s%s", base, path);
    hdr_kv_t hdrs[] = {
        {"client_id", cid},
        {"sign", sig_hex},
        {"t", t_str},
        {"sign_method", "HMAC-SHA256"},
    };
    char *resp = malloc(2048);
    if (!resp) return false;
    int code = http_req_json(url, HTTP_METHOD_GET, hdrs, 4, NULL, resp, 2048);
    cJSON *j = cJSON_Parse(resp);
    free(resp);

    if (code != 200 || !j || !cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
        cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
        snprintf(err_buf, err_sz, "%s", cJSON_IsString(m) ? m->valuestring : "Eroare token Tuya");
        if (j) cJSON_Delete(j);
        return false;
    }
    cJSON *res = cJSON_GetObjectItem(j, "result");
    cJSON *tok = res ? cJSON_GetObjectItem(res, "access_token") : NULL;
    cJSON *ex = res ? cJSON_GetObjectItem(res, "expire_time") : NULL;
    if (cJSON_IsString(tok)) {
        s_tuya_sess.valid = true;
        strncpy(s_tuya_sess.cid, cid, sizeof(s_tuya_sess.cid) - 1);
        strncpy(s_tuya_sess.reg, region, sizeof(s_tuya_sess.reg) - 1);
        strncpy(s_tuya_sess.token, tok->valuestring, sizeof(s_tuya_sess.token) - 1);
        s_tuya_sess.exp = now + (cJSON_IsNumber(ex) ? (time_t)ex->valueint : 3600);
    }
    cJSON_Delete(j);
    return s_tuya_sess.valid;
}

static const char *tcl_int_to_mode(int wm) {
    switch (wm) {
        case 0: return "auto";
        case 1: return "cool";
        case 2: return "dry";
        case 3: return "fan";
        case 4: return "heat";
        default: return "cool";
    }
}

static int tcl_mode_to_int(const char *m) {
    if (strcmp(m, "auto") == 0) return 0;
    if (strcmp(m, "cool") == 0) return 1;
    if (strcmp(m, "dry") == 0) return 2;
    if (strcmp(m, "fan") == 0) return 3;
    if (strcmp(m, "heat") == 0) return 4;
    return 1;
}

static int json_get_int_fallback(cJSON *rep, cJSON *des, const char *k, int def) {
    cJSON *v = rep ? cJSON_GetObjectItem(rep, k) : NULL;
    if (!v || cJSON_IsNull(v)) v = des ? cJSON_GetObjectItem(des, k) : NULL;
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static bool json_has_key(cJSON *rep, cJSON *des, const char *k) {
    return (rep && cJSON_GetObjectItem(rep, k) != NULL) || (des && cJSON_GetObjectItem(des, k) != NULL);
}

bool ac_control_get_state(ac_state_t *out_state, char *err_buf, size_t err_sz) {
    ac_control_init();
    xSemaphoreTake(s_ac_mutex, portMAX_DELAY);

    if (strcmp(g_settings.ac_brand, "tcl") == 0) {
        if (!g_settings.ac_tcl_email[0] || !g_settings.ac_tcl_password[0] || !g_settings.ac_selected_device_id[0]) {
            snprintf(err_buf, err_sz, "AC TCL neconfigurat");
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        if (!tcl_ensure_session(g_settings.ac_tcl_email, g_settings.ac_tcl_password, false, err_buf, err_sz)) {
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        char *resp = malloc(8192);
        if (!resp) {
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        int code = tcl_shadow_req(g_settings.ac_selected_device_id, false, NULL, resp, 8192);
        if (code == 401 || code == 403) {
            if (tcl_ensure_session(g_settings.ac_tcl_email, g_settings.ac_tcl_password, true, err_buf, err_sz)) {
                code = tcl_shadow_req(g_settings.ac_selected_device_id, false, NULL, resp, 8192);
            }
        }
        if (code != 200) {
            snprintf(err_buf, err_sz, "Eroare citire AC TCL (HTTP %d)", code);
            free(resp);
            xSemaphoreGive(s_ac_mutex);
            return false;
        }

        cJSON *root = cJSON_Parse(resp);
        free(resp);
        cJSON *st = root ? cJSON_GetObjectItem(root, "state") : NULL;
        cJSON *rep = st ? cJSON_GetObjectItem(st, "reported") : NULL;
        cJSON *des = st ? cJSON_GetObjectItem(st, "desired") : NULL;

        ac_state_t res = {.valid = true, .target_temp = 23, .fan_max = 7};
        res.power = (json_get_int_fallback(rep, des, "powerSwitch", 0) == 1);
        res.target_temp = json_get_int_fallback(rep, des, "targetTemperature", 23);
        cJSON *ct = rep ? cJSON_GetObjectItem(rep, "currentTemperature") : NULL;
        if (!cJSON_IsNumber(ct) && des) ct = cJSON_GetObjectItem(des, "currentTemperature");
        if (cJSON_IsNumber(ct)) {
            res.has_current_temp = true;
            res.current_temp = (float)ct->valuedouble;
        }
        int wm = json_get_int_fallback(rep, des, "workMode", 1);
        strncpy(res.mode, tcl_int_to_mode(wm), sizeof(res.mode) - 1);

        if (json_has_key(rep, des, "windSpeed7Gear") || json_has_key(rep, des, "windSpeedAutoSwitch")) {
            strcpy(res.fan_type, "7gear");
            int auto_sw = json_get_int_fallback(rep, des, "windSpeedAutoSwitch", 0);
            int g7 = json_get_int_fallback(rep, des, "windSpeed7Gear", 0);
            res.fan_speed = (auto_sw == 1 || g7 == 0) ? 0 : (g7 < 1 ? 1 : (g7 > 7 ? 7 : g7));
        } else {
            strcpy(res.fan_type, "standard");
            int turbo = json_get_int_fallback(rep, des, "turbo", 0);
            int sil = json_get_int_fallback(rep, des, "silenceSwitch", 0);
            int ws = json_get_int_fallback(rep, des, "windSpeed", 0);
            if (turbo == 1) res.fan_speed = 7;
            else if (sil == 1) res.fan_speed = 1;
            else if (ws == 0) res.fan_speed = 0;
            else res.fan_speed = (ws < 1 ? 1 : (ws > 6 ? 6 : ws));
        }
        if (root) cJSON_Delete(root);
        s_last_state = res;
        *out_state = res;
        xSemaphoreGive(s_ac_mutex);
        return true;
    }

    if (strcmp(g_settings.ac_brand, "tuya") == 0) {
        if (!tuya_get_token(g_settings.ac_tuya_region, g_settings.ac_tuya_client_id, g_settings.ac_tuya_client_secret, false, err_buf, err_sz)) {
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        const char *base = "https://openapi.tuyaeu.com";
        if (strcmp(g_settings.ac_tuya_region, "us") == 0) base = "https://openapi.tuyaus.com";
        else if (strcmp(g_settings.ac_tuya_region, "cn") == 0) base = "https://openapi.tuyacn.com";
        else if (strcmp(g_settings.ac_tuya_region, "in") == 0) base = "https://openapi.tuyain.com";

        char path[128], url[192], t_str[24], empty_hash[65], sign_str[384], sig_hex[65];
        snprintf(path, sizeof(path), "/v1.0/iot-03/devices/%s/status", g_settings.ac_tuya_device_id);
        snprintf(url, sizeof(url), "%s%s", base, path);
        snprintf(t_str, sizeof(t_str), "%lld", (long long)time(NULL) * 1000LL);
        calc_sha256_hex((const uint8_t *)"", 0, empty_hash);
        snprintf(sign_str, sizeof(sign_str), "%s%s%sGET\n%s\n\n%s",
                 g_settings.ac_tuya_client_id, s_tuya_sess.token, t_str, empty_hash, path);
        uint8_t sig[32];
        calc_hmac_sha256((const uint8_t *)g_settings.ac_tuya_client_secret, strlen(g_settings.ac_tuya_client_secret),
                         (const uint8_t *)sign_str, strlen(sign_str), sig);
        bytes_to_hex(sig, 32, sig_hex, true);
        hdr_kv_t hdrs[] = {
            {"client_id", g_settings.ac_tuya_client_id},
            {"access_token", s_tuya_sess.token},
            {"sign", sig_hex},
            {"t", t_str},
            {"sign_method", "HMAC-SHA256"},
        };
        char *resp = malloc(4096);
        if (!resp) { xSemaphoreGive(s_ac_mutex); return false; }
        int code = http_req_json(url, HTTP_METHOD_GET, hdrs, 5, NULL, resp, 4096);
        cJSON *j = cJSON_Parse(resp);
        free(resp);
        if (code != 200 || !j || !cJSON_IsTrue(cJSON_GetObjectItem(j, "success"))) {
            cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
            snprintf(err_buf, err_sz, "%s", cJSON_IsString(m) ? m->valuestring : "Eroare Tuya");
            if (j) cJSON_Delete(j);
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        ac_state_t res = {.valid = true, .target_temp = 23, .mode = "cool", .fan_max = 7};
        cJSON *arr = cJSON_GetObjectItem(j, "result");
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, arr) {
            cJSON *c = cJSON_GetObjectItem(it, "code");
            cJSON *v = cJSON_GetObjectItem(it, "value");
            if (!cJSON_IsString(c) || !v) continue;
            if (strcmp(c->valuestring, "switch") == 0 || strcmp(c->valuestring, "Power") == 0) {
                res.power = cJSON_IsTrue(v);
            } else if (strcmp(c->valuestring, "temp_set") == 0 || strcmp(c->valuestring, "SetTemp") == 0) {
                int tv = v->valueint;
                res.target_temp = (tv > 50) ? (tv / 10) : tv;
            } else if (strcmp(c->valuestring, "temp_current") == 0 || strcmp(c->valuestring, "CurTemp") == 0) {
                double cv = v->valuedouble;
                res.has_current_temp = true;
                res.current_temp = (float)((cv > 50.0) ? (cv / 10.0) : cv);
            } else if ((strcmp(c->valuestring, "mode") == 0 || strcmp(c->valuestring, "Mode") == 0) && cJSON_IsString(v)) {
                strncpy(res.mode, v->valuestring, sizeof(res.mode) - 1);
            }
        }
        cJSON_Delete(j);
        s_last_state = res;
        *out_state = res;
        xSemaphoreGive(s_ac_mutex);
        return true;
    }

    snprintf(err_buf, err_sz, "Brand AC neconfigurat");
    xSemaphoreGive(s_ac_mutex);
    return false;
}

bool ac_control_send_cmd(const ac_cmd_t *cmd, ac_state_t *out_state, char *err_buf, size_t err_sz) {
    ac_control_init();
    xSemaphoreTake(s_ac_mutex, portMAX_DELAY);

    if (strcmp(g_settings.ac_brand, "tcl") == 0) {
        if (!tcl_ensure_session(g_settings.ac_tcl_email, g_settings.ac_tcl_password, false, err_buf, err_sz)) {
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *st = cJSON_AddObjectToObject(root, "state");
        cJSON *des = cJSON_AddObjectToObject(st, "desired");
        if (cmd->has_power) cJSON_AddNumberToObject(des, "powerSwitch", cmd->power ? 1 : 0);
        if (cmd->has_target_temp) {
            int t = cmd->target_temp < 16 ? 16 : (cmd->target_temp > 31 ? 31 : cmd->target_temp);
            cJSON_AddNumberToObject(des, "targetTemperature", t);
        }
        if (cmd->has_mode) {
            cJSON_AddNumberToObject(des, "workMode", tcl_mode_to_int(cmd->mode));
            if (!cmd->has_power) cJSON_AddNumberToObject(des, "powerSwitch", 1);
        }
        if (cmd->has_fan_speed) {
            int fs = cmd->fan_speed < 0 ? 0 : (cmd->fan_speed > 7 ? 7 : cmd->fan_speed);
            if (strcmp(s_last_state.fan_type, "7gear") == 0) {
                cJSON_AddNumberToObject(des, "windSpeedAutoSwitch", fs == 0 ? 1 : 0);
                cJSON_AddNumberToObject(des, "windSpeed7Gear", fs);
            } else {
                cJSON_AddNumberToObject(des, "highTemperatureWind", 0);
                if (fs == 0) {
                    cJSON_AddNumberToObject(des, "windSpeed", 0);
                    cJSON_AddNumberToObject(des, "turbo", 0);
                    cJSON_AddNumberToObject(des, "silenceSwitch", 0);
                } else if (fs == 1) {
                    cJSON_AddNumberToObject(des, "windSpeed", 2);
                    cJSON_AddNumberToObject(des, "turbo", 0);
                    cJSON_AddNumberToObject(des, "silenceSwitch", 1);
                } else if (fs == 7) {
                    cJSON_AddNumberToObject(des, "windSpeed", 6);
                    cJSON_AddNumberToObject(des, "turbo", 1);
                    cJSON_AddNumberToObject(des, "silenceSwitch", 0);
                } else {
                    cJSON_AddNumberToObject(des, "windSpeed", fs);
                    cJSON_AddNumberToObject(des, "turbo", 0);
                    cJSON_AddNumberToObject(des, "silenceSwitch", 0);
                }
            }
        }
        char ctok[32];
        snprintf(ctok, sizeof(ctok), "esp_%lld", (long long)time(NULL));
        cJSON_AddStringToObject(root, "clientToken", ctok);
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        char *resp = malloc(2048);
        if (!resp || !body) { free(resp); free(body); xSemaphoreGive(s_ac_mutex); return false; }
        int code = tcl_shadow_req(g_settings.ac_selected_device_id, true, body, resp, 2048);
        if (code == 401 || code == 403) {
            if (tcl_ensure_session(g_settings.ac_tcl_email, g_settings.ac_tcl_password, true, err_buf, err_sz)) {
                code = tcl_shadow_req(g_settings.ac_selected_device_id, true, body, resp, 2048);
            }
        }
        free(body);
        free(resp);
        if (code != 200) {
            snprintf(err_buf, err_sz, "Eroare comanda AC TCL (HTTP %d)", code);
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        if (cmd->has_power) s_last_state.power = cmd->power;
        if (cmd->has_target_temp) s_last_state.target_temp = cmd->target_temp < 16 ? 16 : (cmd->target_temp > 31 ? 31 : cmd->target_temp);
        if (cmd->has_mode) {
            strncpy(s_last_state.mode, cmd->mode, sizeof(s_last_state.mode) - 1);
            s_last_state.power = true;
        }
        if (cmd->has_fan_speed) s_last_state.fan_speed = cmd->fan_speed < 0 ? 0 : (cmd->fan_speed > 7 ? 7 : cmd->fan_speed);
        s_last_state.valid = true;
        *out_state = s_last_state;
        xSemaphoreGive(s_ac_mutex);
        return true;
    }

    if (strcmp(g_settings.ac_brand, "tuya") == 0) {
        if (!tuya_get_token(g_settings.ac_tuya_region, g_settings.ac_tuya_client_id, g_settings.ac_tuya_client_secret, false, err_buf, err_sz)) {
            xSemaphoreGive(s_ac_mutex);
            return false;
        }
        cJSON *root = cJSON_CreateObject();
        cJSON *cmds = cJSON_AddArrayToObject(root, "commands");
        if (cmd->has_power) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "code", "switch");
            cJSON_AddBoolToObject(o, "value", cmd->power);
            cJSON_AddItemToArray(cmds, o);
        }
        if (cmd->has_target_temp) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "code", "temp_set");
            cJSON_AddNumberToObject(o, "value", cmd->target_temp < 16 ? 16 : (cmd->target_temp > 31 ? 31 : cmd->target_temp));
            cJSON_AddItemToArray(cmds, o);
        }
        if (cmd->has_mode) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "code", "mode");
            cJSON_AddStringToObject(o, "value", cmd->mode);
            cJSON_AddItemToArray(cmds, o);
        }
        char *body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        const char *base = "https://openapi.tuyaeu.com";
        if (strcmp(g_settings.ac_tuya_region, "us") == 0) base = "https://openapi.tuyaus.com";
        else if (strcmp(g_settings.ac_tuya_region, "cn") == 0) base = "https://openapi.tuyacn.com";
        else if (strcmp(g_settings.ac_tuya_region, "in") == 0) base = "https://openapi.tuyain.com";

        char path[128], url[192], t_str[24], bhash[65], sign_str[384], sig_hex[65];
        snprintf(path, sizeof(path), "/v1.0/iot-03/devices/%s/commands", g_settings.ac_tuya_device_id);
        snprintf(url, sizeof(url), "%s%s", base, path);
        snprintf(t_str, sizeof(t_str), "%lld", (long long)time(NULL) * 1000LL);
        calc_sha256_hex((const uint8_t *)body, strlen(body), bhash);
        snprintf(sign_str, sizeof(sign_str), "%s%s%sPOST\n%s\n\n%s",
                 g_settings.ac_tuya_client_id, s_tuya_sess.token, t_str, bhash, path);
        uint8_t sig[32];
        calc_hmac_sha256((const uint8_t *)g_settings.ac_tuya_client_secret, strlen(g_settings.ac_tuya_client_secret),
                         (const uint8_t *)sign_str, strlen(sign_str), sig);
        bytes_to_hex(sig, 32, sig_hex, true);
        hdr_kv_t hdrs[] = {
            {"client_id", g_settings.ac_tuya_client_id},
            {"access_token", s_tuya_sess.token},
            {"sign", sig_hex},
            {"t", t_str},
            {"sign_method", "HMAC-SHA256"},
            {"content-type", "application/json"},
        };
        char *resp = malloc(2048);
        int code = http_req_json(url, HTTP_METHOD_POST, hdrs, 6, body, resp, 2048);
        free(body);
        cJSON *j = cJSON_Parse(resp);
        free(resp);
        bool ok = (code == 200 && j && cJSON_IsTrue(cJSON_GetObjectItem(j, "success")));
        if (!ok) {
            cJSON *m = j ? cJSON_GetObjectItem(j, "msg") : NULL;
            snprintf(err_buf, err_sz, "%s", cJSON_IsString(m) ? m->valuestring : "Eroare comanda Tuya");
        }
        if (j) cJSON_Delete(j);
        if (!ok) { xSemaphoreGive(s_ac_mutex); return false; }

        if (cmd->has_power) s_last_state.power = cmd->power;
        if (cmd->has_target_temp) s_last_state.target_temp = cmd->target_temp;
        if (cmd->has_mode) { strncpy(s_last_state.mode, cmd->mode, sizeof(s_last_state.mode) - 1); s_last_state.power = true; }
        s_last_state.valid = true;
        *out_state = s_last_state;
        xSemaphoreGive(s_ac_mutex);
        return true;
    }

    snprintf(err_buf, err_sz, "Brand AC neconfigurat");
    xSemaphoreGive(s_ac_mutex);
    return false;
}
