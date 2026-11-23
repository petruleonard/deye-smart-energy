#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cpu_pct;
    int cpu_mhz;
    int ram_pct;
    uint32_t ram_used_kb;
    uint32_t ram_total_kb;
    int psram_pct;
    int storage_pct;
    uint32_t storage_used_kb;
    uint32_t storage_total_kb;
    uint32_t spiffs_used_kb;
    uint32_t spiffs_total_kb;
    float esp_temp_c;
    bool esp_temp_ok;
} sys_metrics_t;

void sys_metrics_init(void);
void sys_metrics_get(sys_metrics_t *out);

#ifdef __cplusplus
}
#endif
