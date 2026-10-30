#include "sdkconfig.h"

#if CONFIG_HY_DEBUG_CONSOLE

#include "console.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_CMDS 48
#define LINE_MAX_LEN 96
/* stdin is the USB Serial/JTAG VFS without the driver: a read never blocks, and without a USB host it returns EOF at
 * once. With a host, poll every 20 ms so commands answer promptly; light sleep is off on VBUS anyway. Without one
 * (battery), poll every 2 s: a 20 ms wait is 2 ticks at 100 Hz, under CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP (3),
 * and kept tickless idle from ever entering light sleep. After a host connects, the first command waits at most
 * one slow poll; the host holds the bytes until the FIFO is read, so nothing is lost. */
#define POLL_HOST_MS 20
#define POLL_NO_HOST_MS 2000

static const char *TAG = "console";

static struct {
    const char *name;
    void (*fn)(const char *args);
} s_cmds[MAX_CMDS];
static int s_ncmds;

void console_register(const char *name, void (*fn)(const char *args))
{
    if (s_ncmds < MAX_CMDS) {
        s_cmds[s_ncmds].name = name;
        s_cmds[s_ncmds].fn = fn;
        s_ncmds++;
    } else {
        ESP_LOGE(TAG, "command table full (%d), dropped: %s", MAX_CMDS, name);
    }
}

static void dispatch(char *line)
{
    char *args = line;
    while (*args && *args != ' ') {
        args++;
    }
    if (*args) {
        *args++ = '\0';
        while (*args == ' ') {
            args++;
        }
    }
    if (!*line) {
        return;
    }
    for (int i = 0; i < s_ncmds; i++) {
        if (strcmp(line, s_cmds[i].name) == 0) {
            ESP_LOGI(TAG, "run %s", line);
            s_cmds[i].fn(args);
            return;
        }
    }
    ESP_LOGW(TAG, "unknown command: %s", line);
}

static void console_task(void *arg)
{
    char line[LINE_MAX_LEN];
    size_t n = 0;
    bool overflow = false;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(usb_serial_jtag_is_connected() ? POLL_HOST_MS : POLL_NO_HOST_MS));
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (overflow) {
                ESP_LOGW(TAG, "line too long (max %d), discarded", LINE_MAX_LEN - 1);
            } else {
                line[n] = '\0';
                dispatch(line);
            }
            n = 0;
            overflow = false;
        } else if (n < sizeof line - 1) {
            line[n++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

void console_start(void)
{
    if (xTaskCreate(console_task, "console", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    }
}

#endif /* CONFIG_HY_DEBUG_CONSOLE */
