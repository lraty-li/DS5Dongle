#ifndef DS5_HAPTICS_MAILBOX_H
#define DS5_HAPTICS_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ds5_haptics_mailbox_init(void);
bool ds5_haptics_mailbox_publish(const uint8_t *data, size_t length);
bool ds5_haptics_mailbox_try_receive(uint8_t *data, size_t capacity);
uint32_t ds5_haptics_mailbox_dropped_count(void);

#ifdef __cplusplus
}
#endif

#endif
