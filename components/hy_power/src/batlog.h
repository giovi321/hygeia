#pragma once

#include <stdint.h>

#include "hy/power.h"
#include "sdkconfig.h"

/* <storage>/batlog.csv, header ts,mv,pct,raw_pct,vbus,chg,scr. At 256 KB the file becomes batlog.1.csv
 * (replacing it) and a new one starts. Safe from any task. */
void batlog_init(void); /* once, before any other call */
void batlog_append(int64_t ts, const hy_batt_raw_t *raw, int8_t pct, uint8_t scr);

#if CONFIG_HY_DEBUG_CONSOLE
void batlog_print(void); /* batlog.1.csv, then batlog.csv */
void batlog_clear(void);
#endif
