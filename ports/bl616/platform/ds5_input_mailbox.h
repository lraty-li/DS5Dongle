#ifndef DS5_INPUT_MAILBOX_H
#define DS5_INPUT_MAILBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int ds5_input_mailbox_init(void);
bool ds5_input_mailbox_publish(const uint8_t *payload, size_t length);
bool ds5_input_mailbox_receive(uint8_t *payload, size_t capacity,
                               uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
