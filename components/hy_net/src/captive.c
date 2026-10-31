#include "net_priv.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/dns.h"
#include "lwip/sockets.h"

#define TASK_STACK 3072
#define TASK_PRIO  3
#define RX_TIMEOUT_MS 500
#define DNS_MAX 512

static const char *TAG = "captive";

static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_stop;
static uint32_t s_ip_be;
/* One task at a time: the buffers stay off its stack */
static uint8_t s_q[DNS_MAX], s_a[DNS_MAX + 16];

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        goto out;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        ESP_LOGE(TAG, "bind port 53: errno %d", errno);
        close(sock);
        goto out;
    }
    struct timeval tv = {.tv_sec = 0, .tv_usec = RX_TIMEOUT_MS * 1000};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ESP_LOGI(TAG, "dns on port 53");
    while (!s_stop) {
        struct sockaddr_in from;
        socklen_t flen = sizeof from;
        int n = recvfrom(sock, s_q, sizeof s_q, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            continue;   /* timeout: check the stop flag */
        }
        size_t len = hy_dns_answer(s_q, (size_t)n, s_ip_be, s_a, sizeof s_a);
        if (len) {
            sendto(sock, s_a, len, 0, (struct sockaddr *)&from, flen);
        }
    }
    close(sock);
    ESP_LOGI(TAG, "dns stopped");
out:
    s_task = NULL;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t net_captive_start(uint32_t ip_be)
{
    if (s_task) {
        return ESP_OK;
    }
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_done, 0);   /* clear a stale give */
    s_ip_be = ip_be;
    s_stop = false;
    if (xTaskCreate(dns_task, "captive", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void net_captive_stop(void)
{
    if (!s_task) {
        return;
    }
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(RX_TIMEOUT_MS * 4)) != pdTRUE) {
        ESP_LOGE(TAG, "dns task did not stop");
    }
}
