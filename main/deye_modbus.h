#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t deye_modbus_init(void);
void deye_modbus_set_scan_paused(bool paused);
bool deye_modbus_is_scan_paused(void);

#ifdef __cplusplus
}
#endif

