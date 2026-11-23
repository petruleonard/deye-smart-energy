#include "deye_data.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static deye_data_t s_telemetry = {
    .wifi_connected = false,
    .logger_connected = false,
    .status_str = "Inițializare..."
};

static SemaphoreHandle_t s_data_mutex = NULL;

void deye_data_init(void)
{
    if (s_data_mutex == NULL) {
        s_data_mutex = xSemaphoreCreateMutex();
    }
}

void deye_data_get(deye_data_t *out)
{
    if (!out) return;
    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        memcpy(out, &s_telemetry, sizeof(deye_data_t));
        xSemaphoreGive(s_data_mutex);
    } else {
        memcpy(out, &s_telemetry, sizeof(deye_data_t));
    }
}

void deye_data_update(const deye_data_t *in)
{
    if (!in) return;
    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        memcpy(&s_telemetry, in, sizeof(deye_data_t));
        xSemaphoreGive(s_data_mutex);
    } else {
        memcpy(&s_telemetry, in, sizeof(deye_data_t));
    }
}

