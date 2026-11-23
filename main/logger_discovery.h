#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ip[16];
    char mac[20];
    uint32_t sn;
    char model[32];
} discovered_logger_t;

// Discovers a Solarman/Deye Wi-Fi logger on the LAN via UDP broadcast (port 48899).
// Matches by normalized MAC address (e.g. "AABBCCDDEEFF" or "AA:BB:CC:DD:EE:FF"),
// or by Serial Number if target_mac is empty.
bool logger_discover_by_mac(const char *target_mac, uint32_t target_sn,
                            char *out_ip, size_t out_ip_sz,
                            char *out_mac, size_t out_mac_sz,
                            uint32_t *out_sn);

// Discovers all Solarman loggers on the LAN and probes Modbus reg 0 for model type.
int logger_discover_all(discovered_logger_t *out_list, int max_count);
int logger_discover_all_ex(discovered_logger_t *out_list, int max_count, bool blind_mode);

#ifdef __cplusplus
}
#endif

