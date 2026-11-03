/* Between qmi8658.c (which arms wake on motion) and board.c (which owns the GPIO ISRs and the light-sleep wake set) */
#pragma once

#include "esp_err.h"

/* The IMU interrupt is live on gpio: input with no pull, interrupt on both edges calling the hook, and in the wake
   set while it is armed. *level = the pin level read at attach. */
esp_err_t board_imu_int_attach(int gpio, int *level);
/* Back to the pin's reset state, out of the wake set; no-op when not attached */
void board_imu_int_detach(void);
