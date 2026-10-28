#ifndef HY_PCF85063_H
#define HY_PCF85063_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    uint8_t raw[7];      /* registers 0x04..0x0A as read */
    uint8_t ctrl1;       /* register 0x00 */
    bool osc_stopped;    /* OS flag, seconds register bit 7 */
    bool decoded;        /* BCD and ranges valid, 24 h mode */
    int64_t epoch;       /* UTC seconds, valid when decoded */
} pcf85063_time_t;

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus);
esp_err_t pcf85063_read(pcf85063_time_t *out);
/* Writes registers 0x04..0x0A with the OS flag clear; starts the oscillator and forces 24 h mode */
esp_err_t pcf85063_write(int64_t epoch);

#endif
