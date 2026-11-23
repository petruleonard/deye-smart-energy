#pragma once

#include "esp_err.h"
#include "deye_data.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t telegram_notify_send(const char *text);
esp_err_t telegram_notify_send_test(void);
void telegram_notify_check_grid(const deye_data_t *d);

#ifdef __cplusplus
}
#endif
