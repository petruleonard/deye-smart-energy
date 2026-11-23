#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Solar (PV)
    int32_t pv_power_w;       // PV1 + PV2 Total Power (W)
    int32_t pv1_power_w;      // MPPT1 Power (W)
    int32_t pv2_power_w;      // MPPT2 Power (W)
    int32_t pv3_power_w;      // MPPT3 (0 for 1-phase)
    int32_t pv4_power_w;      // MPPT4 (0 for 1-phase)
    float   pv1_volts;        // MPPT1 Voltage (V)
    float   pv2_volts;        // MPPT2 Voltage (V)
    float   pv_today_kwh;     // Daily Production (kWh)
    float   pv_total_kwh;     // Lifetime Production (kWh)

    // Battery (Baterie)
    int32_t battery_power_w;   // >0 Charging, <0 Discharging (W)
    uint16_t battery_soc;      // State of Charge (%)
    uint16_t battery_soh;      // State of Health (%)
    float    battery_volts;    // Voltage (V)
    float    battery_current;  // Current (A)
    float    battery_temp_c;   // BMS / Battery temp (°C)
    float    battery_capacity_kwh; // Nominal/Configured capacity
    float    battery_charge_today_kwh;
    float    battery_discharge_today_kwh;
    float    battery_charge_total_kwh;
    float    battery_discharge_total_kwh;
    uint16_t bat_max_chg_a;

    // Grid (Rețea)
    int32_t grid_power_w;      // >0 Import from grid, <0 Export to grid (W)
    float   grid_volts;        // L1 Voltage (V)
    float   grid_l2_volts;     // L2 Voltage (V)
    float   grid_l3_volts;     // L3 Voltage (V)
    int32_t grid_l1_power_w;
    int32_t grid_l2_power_w;
    int32_t grid_l3_power_w;
    float   grid_hz;           // Frequency (Hz)
    float   grid_buy_today_kwh;
    float   grid_sell_today_kwh;
    float   grid_buy_total_kwh;
    float   grid_sell_total_kwh;
    uint16_t grid_outages_7d;
    uint16_t grid_outages_14d;
    uint16_t grid_outages_30d;

    // Load (Casă / Consumatori)
    int32_t load_power_w;      // Instantaneous Load (W)
    float   load_today_kwh;    // Daily Consumption (kWh)
    float   load_total_kwh;    // Lifetime Consumption (kWh)
    float   load_peak_today_kw;
    float   load_avg_hourly_kw;
    float   load_pf;           // Power Factor
    char    load_source_str[16]; // "Solar", "Baterie", "Retea"

    // Inverter Info
    uint16_t inverter_status;  // 0:Standby, 1:Self-check, 2:Normal, 3:Alarm, 4:Fault
    float    inverter_temp_c;  // DC/AC Temperature (°C)
    char     inverter_sn[20];  // Modbus regs 3..7 ASCII Serial
    uint32_t inverter_rated_w; // Modbus regs 20..21 Rated Power (W)

    // System Status
    bool wifi_connected;
    bool logger_connected;
    char status_str[64];
    uint32_t last_update_sec;
} deye_data_t;

void deye_data_init(void);
void deye_data_get(deye_data_t *out);
void deye_data_update(const deye_data_t *in);

#ifdef __cplusplus
}
#endif

