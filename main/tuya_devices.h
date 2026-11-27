#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TUYA_KIND_DEHUM = 0,
    TUYA_KIND_SENSOR = 1,
    TUYA_KIND_PLUG = 2,
} tuya_dev_kind_t;

void tuya_devices_init(void);
void tuya_devices_get_fallback_creds(char *reg, size_t reg_sz, char *cid, size_t cid_sz, char *sec, size_t sec_sz);
void tuya_devices_ai_summary(char *out, size_t out_sz);

char *tuya_dev_status_json(tuya_dev_kind_t kind);
char *tuya_dev_discover_json(tuya_dev_kind_t kind, const char *req_body);
char *tuya_dev_config_json(tuya_dev_kind_t kind, const char *req_body);
char *tuya_dev_control_json(tuya_dev_kind_t kind, const char *req_body);

#ifdef __cplusplus
}
#endif
