#ifndef DS5_LOG_H
#define DS5_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ds5_log_init(void);
int ds5_log_printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));

size_t ds5_log_peek(uint8_t *buffer, size_t capacity);
void ds5_log_discard(size_t length);
uint32_t ds5_log_dropped_bytes(void);

#ifdef __cplusplus
}
#endif

#endif
