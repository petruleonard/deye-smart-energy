#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "deye_data.h"

void history_store_init(void);
void history_store_start_sntp(void);
void history_store_record(const deye_data_t *d);
void history_store_query(const char *period, const char *date_str, char *out_json, size_t out_sz);
void history_store_chart_json(char *out_json, size_t out_sz);
void history_store_enable_fast_test(bool enable);
bool history_store_clear(void);
bool history_store_get_storage_info(size_t *total_bytes, size_t *used_bytes);
