#include "solarman_v5.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "solarman_v5";
#define SOLARMAN_PORT 8899

static uint16_t s_seq = 1;
static SemaphoreHandle_t s_solarman_mutex = NULL;

void solarman_v5_init(void)
{
    if (!s_solarman_mutex) {
        s_solarman_mutex = xSemaphoreCreateMutex();
    }
}

static uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (int i = 8; i != 0; i--) {
            if ((crc & 0x0001) != 0) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint8_t v5_checksum(const uint8_t *buf, size_t start, size_t end)
{
    uint32_t sum = 0;
    for (size_t i = start; i < end; i++) {
        sum += buf[i];
    }
    return (uint8_t)(sum & 0xFF);
}

static esp_err_t solarman_v5_transact_once(const char *ip, uint32_t logger_sn,
                                           const uint8_t *mb_frame, uint16_t mb_len,
                                           uint8_t *rx_buf, size_t rx_buf_sz, int *out_rx_len)
{
    if (!ip || strlen(ip) == 0 || logger_sn == 0 || !mb_frame || mb_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t payload_len = 15 + mb_len;
    uint16_t total_v5_len = 11 + payload_len + 2;
    uint8_t tx_buf[80];
    if (total_v5_len > sizeof(tx_buf)) {
        return ESP_ERR_NO_MEM;
    }

    size_t idx = 0;
    tx_buf[idx++] = 0xA5;
    tx_buf[idx++] = (uint8_t)(payload_len & 0xFF);
    tx_buf[idx++] = (uint8_t)((payload_len >> 8) & 0xFF);
    tx_buf[idx++] = 0x10;
    tx_buf[idx++] = 0x45;
    tx_buf[idx++] = (uint8_t)(s_seq & 0xFF);
    tx_buf[idx++] = (uint8_t)((s_seq >> 8) & 0xFF);
    s_seq++;
    if (s_seq == 0) s_seq = 1;

    tx_buf[idx++] = (uint8_t)(logger_sn & 0xFF);
    tx_buf[idx++] = (uint8_t)((logger_sn >> 8) & 0xFF);
    tx_buf[idx++] = (uint8_t)((logger_sn >> 16) & 0xFF);
    tx_buf[idx++] = (uint8_t)((logger_sn >> 24) & 0xFF);

    tx_buf[idx++] = 0x02;
    tx_buf[idx++] = 0x00;
    tx_buf[idx++] = 0x00;
    for (int i = 0; i < 12; i++) tx_buf[idx++] = 0x00;

    memcpy(&tx_buf[idx], mb_frame, mb_len);
    idx += mb_len;

    tx_buf[idx] = v5_checksum(tx_buf, 1, idx);
    idx++;
    tx_buf[idx++] = 0x15;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "socket() failed: errno=%d", errno);
        return ESP_FAIL;
    }

    struct timeval rcv_tv = { .tv_sec = 2, .tv_usec = 800000 };
    struct timeval snd_tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_addr.s_addr = inet_addr(ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SOLARMAN_PORT);

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        int e = errno;
        close(sock);
        ESP_LOGW(TAG, "connect(%s:8899) failed: errno=%d", ip, e);
        return ESP_FAIL;
    }

    if (send(sock, tx_buf, idx, 0) < 0) {
        int e = errno;
        close(sock);
        ESP_LOGW(TAG, "send() failed: errno=%d", e);
        return ESP_FAIL;
    }

    int total_rx = 0;
    int expected_len = 0;
    while (total_rx < (int)rx_buf_sz) {
        int r = recv(sock, rx_buf + total_rx, rx_buf_sz - (size_t)total_rx, 0);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            break;
        }
        total_rx += r;
        if (expected_len == 0 && total_rx >= 3 && rx_buf[0] == 0xA5) {
            uint16_t pl = (uint16_t)rx_buf[1] | ((uint16_t)rx_buf[2] << 8);
            if (pl >= 14 && pl < (uint16_t)rx_buf_sz) {
                expected_len = 11 + (int)pl + 2;
            }
        }
        if (expected_len > 0 && total_rx >= expected_len) {
            total_rx = expected_len;
            break;
        }
    }
    close(sock);

    if (total_rx < 29) {
        ESP_LOGW(TAG, "Solarman recv timeout/short (%d bytes)", total_rx);
        return ESP_ERR_TIMEOUT;
    }
    if (rx_buf[0] != 0xA5 || rx_buf[total_rx - 1] != 0x15) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (rx_buf[total_rx - 2] != v5_checksum(rx_buf, 1, total_rx - 2)) {
        return ESP_ERR_INVALID_CRC;
    }
    if (rx_buf[4] != 0x15) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_rx_len = total_rx;
    return ESP_OK;
}

static esp_err_t solarman_v5_transact(const char *ip, uint32_t logger_sn,
                                      const uint8_t *mb_frame, uint16_t mb_len,
                                      uint8_t *rx_buf, size_t rx_buf_sz, int *out_rx_len)
{
    if (!s_solarman_mutex) {
        solarman_v5_init();
    }
    bool locked = (s_solarman_mutex && xSemaphoreTake(s_solarman_mutex, pdMS_TO_TICKS(6000)) == pdTRUE);

    esp_err_t err = solarman_v5_transact_once(ip, logger_sn, mb_frame, mb_len, rx_buf, rx_buf_sz, out_rx_len);
    if (err == ESP_ERR_TIMEOUT || err == ESP_FAIL || err == ESP_ERR_INVALID_CRC) {
        vTaskDelay(pdMS_TO_TICKS(120));
        err = solarman_v5_transact_once(ip, logger_sn, mb_frame, mb_len, rx_buf, rx_buf_sz, out_rx_len);
    }

    if (locked) {
        xSemaphoreGive(s_solarman_mutex);
    }
    return err;
}

esp_err_t solarman_v5_read_registers(const char *ip, uint32_t logger_sn,
                                    uint16_t start_reg, uint16_t num_regs,
                                    uint16_t *out_regs)
{
    if (!out_regs || num_regs == 0) return ESP_ERR_INVALID_ARG;

    uint8_t mb_frame[8];
    mb_frame[0] = 0x01;
    mb_frame[1] = 0x03;
    mb_frame[2] = (uint8_t)((start_reg >> 8) & 0xFF);
    mb_frame[3] = (uint8_t)(start_reg & 0xFF);
    mb_frame[4] = (uint8_t)((num_regs >> 8) & 0xFF);
    mb_frame[5] = (uint8_t)(num_regs & 0xFF);
    uint16_t crc = modbus_crc16(mb_frame, 6);
    mb_frame[6] = (uint8_t)(crc & 0xFF);
    mb_frame[7] = (uint8_t)((crc >> 8) & 0xFF);

    uint8_t rx_buf[256];
    int rx_len = 0;
    esp_err_t err = solarman_v5_transact(ip, logger_sn, mb_frame, sizeof(mb_frame), rx_buf, sizeof(rx_buf), &rx_len);
    if (err != ESP_OK) return err;

    if (rx_buf[26] != 0x03) {
        ESP_LOGW(TAG, "Modbus read exception or wrong func code: 0x%02X", rx_buf[26]);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t byte_count = rx_buf[27];
    if (byte_count < num_regs * 2 || (28 + byte_count + 4) > rx_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (int i = 0; i < num_regs; i++) {
        out_regs[i] = (uint16_t)((rx_buf[28 + i * 2] << 8) | rx_buf[29 + i * 2]);
    }
    return ESP_OK;
}

esp_err_t solarman_v5_write_register(const char *ip, uint32_t logger_sn,
                                     uint16_t reg, uint16_t val)
{
    uint8_t mb_frame[11];
    mb_frame[0] = 0x01;
    mb_frame[1] = 0x10; // Write Multiple Registers (count=1)
    mb_frame[2] = (uint8_t)((reg >> 8) & 0xFF);
    mb_frame[3] = (uint8_t)(reg & 0xFF);
    mb_frame[4] = 0x00;
    mb_frame[5] = 0x01;
    mb_frame[6] = 0x02;
    mb_frame[7] = (uint8_t)((val >> 8) & 0xFF);
    mb_frame[8] = (uint8_t)(val & 0xFF);
    uint16_t crc = modbus_crc16(mb_frame, 9);
    mb_frame[9] = (uint8_t)(crc & 0xFF);
    mb_frame[10] = (uint8_t)((crc >> 8) & 0xFF);

    uint8_t rx_buf[128];
    int rx_len = 0;
    esp_err_t err = solarman_v5_transact(ip, logger_sn, mb_frame, sizeof(mb_frame), rx_buf, sizeof(rx_buf), &rx_len);
    if (err != ESP_OK) return err;

    if (rx_buf[26] != 0x10) {
        ESP_LOGW(TAG, "Modbus write exception or wrong func code: 0x%02X", rx_buf[26]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGW(TAG, "Modbus write OK: reg %u <= %u", (unsigned)reg, (unsigned)val);
    return ESP_OK;
}
