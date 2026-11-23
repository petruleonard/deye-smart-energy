#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t local_web_server_init(void);
bool github_ota_check_latest(char *out_ver, size_t ver_sz, char *out_url, size_t url_sz, char *out_notes, size_t notes_sz);

#ifdef __cplusplus
}
#endif
