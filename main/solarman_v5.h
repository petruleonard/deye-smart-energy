#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read holding registers from Deye inverter via Solarman V5 stick logger
 *
 * @param ip Logger IP address (e.g. "192.168.1.150")
 * @param logger_sn Logger serial number (e.g. 2301234567)
 * @param start_reg Modbus holding register start address (e.g. 0x0096)
 * @param num_regs Number of registers to read (e.g. 30)
 * @param out_regs Buffer to store received 16-bit registers (must be at least num_regs * sizeof(uint16_t))
 * @return ESP_OK on success, error code otherwise
 */
void solarman_v5_init(void);
esp_err_t solarman_v5_read_registers(const char *ip, uint32_t logger_sn,
                                    uint16_t start_reg, uint16_t num_regs,
                                    uint16_t *out_regs);

esp_err_t solarman_v5_write_register(const char *ip, uint32_t logger_sn,
                                     uint16_t reg, uint16_t val);

#ifdef __cplusplus
}
#endif

