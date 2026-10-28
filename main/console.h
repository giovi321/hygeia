#pragma once

/* Tiny serial console: lines from the console UART, dispatched by first word. */
void console_register(const char *name, void (*fn)(const char *args));
void console_start(void);
