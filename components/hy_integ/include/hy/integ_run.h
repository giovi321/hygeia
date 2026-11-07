#pragma once

/* Integration delivery on the device: event capture from the bus, the outbox, webhooks. One delivery task drives
 * every destination. It runs only while the radio is on for another reason and connected as a client: it never
 * starts the radio, never keeps it on and never counts as activity. */

#include <stdint.h>

#include "esp_err.h"
#include "hy/integ.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* After hy_net_init, hy_integ_cfg_load and the store, before hy_ui_start: creates the capture ring, the delivery
 * task ("integ", 8192-byte internal stack, below the LVGL task) and subscribes to the event bus (under
 * hy_board_lock) and the network. The outbox opens lazily, when the first destination is on. When the clock is
 * invalid at start it captures one clock_invalid event. */
esp_err_t hy_integ_init(void);
/* LVGL task, after hy_integ_cfg_save. Never waits for the delivery task: copies the config into a pending slot
 * under a short mutex and returns; the delivery task takes it at its next wake. */
void      hy_integ_apply(const hy_integ_t *saved);
/* Any task: a copy under a mutex. */
void      hy_integ_status(hy_integ_status_t *out);
/* Any task. Sends one signed test event to the saved webhook at index, outside the outbox and the retry state.
 * ESP_ERR_INVALID_ARG: no hook at index. ESP_ERR_INVALID_STATE: not connected as a client (or no delivery task).
 * ESP_ERR_NOT_FINISHED: a test runs or the heavy-job slot is held. ESP_ERR_NO_MEM: internal heap under the gate.
 * ESP_OK: started; the result lands in the status. */
esp_err_t hy_integ_test(uint8_t index);
/* Status changed (delivery, error, test result). Called in the delivery task, must not block. 2 slots; returns the
 * slot, -1 when full. */
int       hy_integ_subscribe(void (*cb)(void *ctx), void *ctx);

#if CONFIG_HY_DEBUG_CONSOLE
/* "integ" console command: status, and RAM-only overrides that persist nothing:
 * hook <url> [secret] | fire <event> [code] | fill <n> | burst <n> | kick | sign <secret> <text> | tcp <host> <port>
 * | off */
void      hy_integ_debug(const char *args);
#endif

#ifdef __cplusplus
}
#endif
