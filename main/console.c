#include "console.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_CMDS 8
#define LINE_MAX_LEN 96

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
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            line[n] = '\0';
            n = 0;
            dispatch(line);
        } else if (n < sizeof line - 1) {
            line[n++] = (char)c;
        }
    }
}

void console_start(void)
{
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
