/*
 * LVGL memory backend. Objects, styles and small buffers go to PSRAM first and
 * fall back to internal RAM, which is kept for Wi-Fi, DMA buffers and task stacks.
 * Provides the same functions as LVGL's C library backend (LV_USE_CUSTOM_MALLOC).
 */

#include "lvgl.h"

#include "esp_heap_caps.h"

#define LV_CAPS_PREFERRED (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_CAPS_FALLBACK  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, LV_CAPS_PREFERRED, LV_CAPS_FALLBACK);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, LV_CAPS_PREFERRED, LV_CAPS_FALLBACK);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

/* Reports the whole PSRAM heap only: not LVGL's share of it, and not LVGL blocks that fell back to internal RAM. */
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    lv_memzero(mon_p, sizeof(*mon_p));
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_cnt = info.free_blocks;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->max_used = mon_p->total_size - info.minimum_free_bytes;
    if (mon_p->total_size > 0) {
        mon_p->used_pct = (uint8_t)(100U - (uint64_t)100U * mon_p->free_size / mon_p->total_size);
    }
    if (mon_p->free_size > 0) {
        mon_p->frag_pct = (uint8_t)(100U - (uint64_t)mon_p->free_biggest_size * 100U / mon_p->free_size);
    }
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}
