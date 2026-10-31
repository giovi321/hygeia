#include "net_priv.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hy/dns.h"
#include "lwip/sockets.h"

#define TASK_STACK 3072
#define TASK_PRIO  3
#define RX_TIMEOUT_MS 500
#define ERR_DELAY_MS  200      /* after a socket error other than the receive timeout */
#define DNS_MAX 512
#define REPLY_MAX (DNS_MAX + 16)   /* the question plus one A record */

static const char *TAG = "captive";

static TaskHandle_t s_task;        /* non-NULL from start until the task has exited */
static SemaphoreHandle_t s_done;   /* given by the task as it exits */
static volatile bool s_stop;
static uint32_t s_ip_be;
static uint8_t *s_q, *s_a;         /* PSRAM while the task runs */

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
        int n = recvfrom(sock, s_q, DNS_MAX, 0, (struct sockaddr *)&from, &flen);
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(ERR_DELAY_MS));   /* no busy loop above the power task */
            }
            continue;
        }
        if (n == 0) {
            continue;
        }
        size_t len = hy_dns_answer(s_q, (size_t)n, s_ip_be, s_a, REPLY_MAX);
        if (len) {
            sendto(sock, s_a, len, 0, (struct sockaddr *)&from, flen);
        }
    }
    close(sock);
    ESP_LOGI(TAG, "dns stopped");
out:
    heap_caps_free(s_q);
    heap_caps_free(s_a);
    s_q = s_a = NULL;
    s_task = NULL;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

/* Waits for an exiting task; true when none is left */
static bool wait_exit(void)
{
    if (!s_task) {
        return true;
    }
    s_stop = true;
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(RX_TIMEOUT_MS * 4)) == pdTRUE) {
        return true;
    }
    return s_task == NULL;
}

esp_err_t net_captive_start(uint32_t ip_be)
{
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
        if (!s_done) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_task && !s_stop) {
        return ESP_OK;   /* running */
    }
    if (!wait_exit()) {
        ESP_LOGE(TAG, "previous dns task still running");
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_done, 0);   /* clear a stale give */
    s_q = heap_caps_malloc(DNS_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_a = heap_caps_malloc(REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_q || !s_a) {
        heap_caps_free(s_q);
        heap_caps_free(s_a);
        s_q = s_a = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_ip_be = ip_be;
    s_stop = false;
    if (xTaskCreate(dns_task, "captive", TASK_STACK, NULL, TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        heap_caps_free(s_q);
        heap_caps_free(s_a);
        s_q = s_a = NULL;
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* A task that does not stop within 2 s keeps its stop flag; the next start waits for it again */
void net_captive_stop(void)
{
    if (!wait_exit()) {
        ESP_LOGE(TAG, "dns task did not stop");
    }
}
