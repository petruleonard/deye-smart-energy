#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool power;
    int target_temp;
    bool has_current_temp;
    float current_temp;
    char mode[16];      // "cool", "heat", "auto", "dry", "fan"
    int fan_speed;      // 0 = Auto, 1..7
    int fan_max;        // 7
    char fan_type[16];  // "standard" or "7gear"
} ac_state_t;

typedef struct {
    bool has_power;
    bool power;
    bool has_target_temp;
    int target_temp;
    bool has_mode;
    char mode[16];
    bool has_fan_speed;
    int fan_speed;
} ac_cmd_t;

void ac_control_init(void);
void ac_control_clear_cache(void);

bool ac_control_connect_tcl(const char *email, const char *password,
                            char *devices_json, size_t devices_json_sz,
                            char *first_dev_id, size_t first_dev_id_sz,
                            char *err_buf, size_t err_sz);

bool ac_control_connect_tuya(const char *region, const char *cid, const char *secret, const char *device_id,
                             char *devices_json, size_t devices_json_sz,
                             char *sel_dev_id, size_t sel_dev_id_sz,
                             char *err_buf, size_t err_sz);

bool ac_control_get_state(ac_state_t *out_state, char *err_buf, size_t err_sz);
bool ac_control_send_cmd(const ac_cmd_t *cmd, ac_state_t *out_state, char *err_buf, size_t err_sz);

bool ac_tuya_get_token(const char *region, const char *cid, const char *sec, bool force, char *err_buf, size_t err_sz);
int ac_tuya_signed_req(const char *region, const char *cid, const char *sec,
                       int method, const char *path_with_q, const char *body,
                       char *resp_buf, size_t resp_sz);
bool ac_tuya_discover_ac(const char *region, const char *cid, const char *sec,
                         char *out_devices_json, size_t out_sz,
                         char *err_buf, size_t err_sz);

#ifdef __cplusplus
}
#endif
